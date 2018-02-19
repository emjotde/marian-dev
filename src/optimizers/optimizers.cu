#include "optimizers.h"

#include "functional/functional.h"
#include "kernels/tensor_operators.h"

namespace marian {
void Sgd::updateImpl(Tensor params, Tensor grads) {
  using namespace functional;
  Element(_1 -= (multiplyFactor_ * eta_) * _2, params, grads);

  cudaStreamSynchronize(0);
}

void Adagrad::updateImpl(Tensor params, Tensor grads) {
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!gt_) {
    int elements = params->size();
    alloc_->reserveExact(params->memory()->size());
    alloc_->allocate(gt_, {1, elements});
    gt_->set(0);
  }

  using namespace functional;

  Element(_1 += (_2 * _2), gt_, grads);

  Element(_1 -= ((multiplyFactor_ * eta_) / (sqrt(_2) + eps_)) * _3,
          params,
          gt_,
          grads);

  cudaStreamSynchronize(0);
}

void Adagrad::resetStats() {
  if(gt_)
    gt_->set(0);
  cudaStreamSynchronize(0);
}

void Adam::load(const std::string& name,
                std::vector<Ptr<OptimizerBase>> opts,
                std::vector<Ptr<Backend>> backends) {
  if(!boost::filesystem::exists(name))
    return;

  LOG(info, "Loading Adam parameters from {}", name);

  std::vector<float> vMt;
  std::vector<float> vVt;
  size_t totalSize = 0;

  auto numpy = cnpy::npz_load(name);
  for(auto it : numpy) {
    auto name = it.first;
    cnpy::NpyArray& np = it.second;

    // get the size of mt_ and vt_, they are the same
    totalSize = np.shape[1];

    // extract data into vectors
    if(name == "adam_mt") {
      vMt.resize(totalSize);
      std::copy((float*)np.data, (float*)np.data + totalSize, vMt.begin());
    }
    if(name == "adam_vt") {
      vVt.resize(totalSize);
      std::copy((float*)np.data, (float*)np.data + totalSize, vVt.begin());
    }
  }

  if(vMt.empty() || vVt.empty()) {
    LOG(info, "[warn] Adam parameters not found in .npz file");
    return;
  }

  size_t shardSize = ceil(totalSize / (float)backends.size());

  size_t id = 0;
  for(auto optBase : opts) {
    auto opt = std::dynamic_pointer_cast<Adam>(optBase);

    int size = std::min(shardSize, totalSize);
    totalSize -= size;

    if(!opt->mt_ || !opt->vt_) {
      if(!opt->alloc_)
        opt->alloc_ = New<TensorAllocator>(backends[id]);

      opt->alloc_->reserveExact(2 * sizeof(float) * size);
      opt->alloc_->allocate(opt->mt_, {1, size});
      opt->alloc_->allocate(opt->vt_, {1, size});
    }

    int shift = id * shardSize;
    std::vector<float> tmpMt(vMt.begin() + shift, vMt.begin() + shift + size);
    opt->mt_->set(tmpMt);
    std::vector<float> tmpVt(vVt.begin() + shift, vVt.begin() + shift + size);
    opt->vt_->set(tmpVt);

    id++;
  }
}

void Adam::save(const std::string& name,
                std::vector<Ptr<OptimizerBase>> opts,
                size_t totalSize) {
  LOG(info, "Saving Adam parameters to {}", name);

  std::vector<float> vMt;
  std::vector<float> vVt;

  for(auto optBase : opts) {
    auto opt = std::dynamic_pointer_cast<Adam>(optBase);

    std::vector<float> tmpMt;
    opt->mt_->get(tmpMt);
    vMt.insert(vMt.end(), tmpMt.begin(), tmpMt.end());

    std::vector<float> tmpVt;
    opt->vt_->get(tmpVt);
    vVt.insert(vVt.end(), tmpVt.begin(), tmpVt.end());
  }

  // truncate to the real size
  if(totalSize < vMt.size()) {
    vMt.resize(totalSize);
    vVt.resize(totalSize);
  }

  // the shape is the same for mt_ and vt_
  unsigned* shape = new unsigned[2];

  shape[0] = 1;
  shape[1] = vMt.size();

  cnpy::npz_save(name, "adam_mt", vMt.data(), shape, 2, "w");
  cnpy::npz_save(name, "adam_vt", vVt.data(), shape, 2, "a");

  delete[] shape;
}

void Adam::updateImpl(Tensor params, Tensor grads) {
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!mt_) {
    int elements = params->size();
    alloc_->reserveExact(2 * params->memory()->size());
    alloc_->allocate(mt_, {1, elements});
    mt_->set(0);

    alloc_->allocate(vt_, {1, elements});
    vt_->set(0);
  }

  t_++;
  float denom1 = 1 - std::pow(beta1_, t_);
  float denom2 = 1 - std::pow(beta2_, t_);

  using namespace functional;

  Element(_1 = (beta1_ * _1) + ((1 - beta1_) * _2), mt_, grads);
  Element(_1 = (beta2_ * _1) + ((1 - beta2_) * (_2 * _2)), vt_, grads);

  Element(_1 -= (multiplyFactor_ * eta_) * (_2 / denom1)
                / (sqrt(_3 / denom2) + eps_),
          params,
          mt_,
          vt_);

  cudaStreamSynchronize(0);
}

void Adam::resetStats() {
  if(mt_)
    mt_->set(0);

  if(vt_)
    vt_->set(0);

  cudaStreamSynchronize(0);
}

Ptr<OptimizerBase> Optimizer(Ptr<Config> options) {
  float lrate = options->get<double>("learn-rate");
  auto params = options->has("optimizer-params")
                    ? options->get<std::vector<float>>("optimizer-params")
                    : std::vector<float>({});

  Ptr<ClipperBase> clipper = nullptr;
  float clipNorm = options->get<double>("clip-norm");
  if(clipNorm > 0)
    clipper = Clipper<Norm>(clipNorm);

  auto opt = options->get<std::string>("optimizer");

  if(opt == "sgd") {
    return Optimizer<Sgd>(lrate, clipper, params);
  } else if(opt == "adagrad") {
    return Optimizer<Adagrad>(lrate, clipper, params);
  } else if(opt == "adam") {
    return Optimizer<Adam>(lrate, clipper, params);
  } else {
    ABORT("Unknown optimizer: {}", opt);
  }
}
}
