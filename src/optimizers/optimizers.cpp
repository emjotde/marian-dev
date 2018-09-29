#include "optimizers.h"

#include "common/io.h"
#include "tensors/tensor_operators.h"

namespace marian {

void Sgd::updateImpl(Tensor params, Tensor grads) {
  using namespace functional;
  Element(_1 -= (multiplyFactor_ * eta_) * _2, params, grads);

  params->getBackend()->synchronize();
}

// Aagrad

void Adagrad::updateImpl(Tensor params, Tensor grads) {
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!gt_) {
    int elements = (int)params->size();
    alloc_->reserveExact(params->memory()->size());
    alloc_->allocate(gt_, {1, elements});
    gt_->set(0.f);
  }

  using namespace functional;

  Element(_1 += (_2 * _2), gt_, grads);

  Element(_1 -= ((multiplyFactor_ * eta_) / (sqrt(_2) + eps_)) * _3,
          params,
          gt_,
          grads);

  params->getBackend()->synchronize();
}

void Adagrad::load(const std::string& name,
                   std::vector<Ptr<OptimizerBase>> opts,
                   std::vector<Ptr<Backend>> backends) {
  ABORT_IF(opts.size() != backends.size(), "opts and backends of different sizes??");

  if(!boost::filesystem::exists(name))
    return;

  LOG(info, "Loading Adagrad parameters from {}", name);

  std::vector<float> vGt;

  // @TODO: use new IO
  auto items = io::loadItems(name);
  for(auto item : items) {
    // get the size of gt_
    auto totalSize = item.shape.elements();

    // extract data into vectors
    if(item.name == "adagrad_gt") {
      vGt.resize(totalSize);
      std::copy(
          (float*)item.data(), (float*)item.data() + totalSize, vGt.begin());
    }
  }
  if(vGt.empty()) {
    LOG(warn, "[warn] Adagrad parameters not found in .npz file");
    return;
  }

  for(size_t id = 0; id < opts.size(); id++) {
    auto opt = std::dynamic_pointer_cast<Adagrad>(opts[id]);

    size_t totalSize = vGt.size();
    size_t shardSize = (size_t)(ceil(totalSize / (float)opts.size()));
    size_t shift = id * shardSize;
    size_t size = std::min(shardSize, totalSize-shift);

    if(!opt->alloc_)
      opt->alloc_ = New<TensorAllocator>(backends[id]);

    if(!opt->gt_) {
      opt->alloc_->reserveExact(sizeof(float) * size);
      opt->alloc_->allocate(opt->gt_, {1, (int)size});
    }

    std::vector<float> tmp(vGt.begin() + shift, vGt.begin() + shift + size);
    opt->gt_->set(tmp);
  }
}

void Adagrad::save(const std::string& name,
                   std::vector<Ptr<OptimizerBase>> opts,
                   size_t /*totalSize*/) {
  LOG(info, "Saving Adagrad parameters to {}", name);

  // fetch and concatenate state vectors from shards into a CPU-side vector
  std::vector<float> vGt;
  for(auto optBase : opts) {
    auto opt = std::dynamic_pointer_cast<Adagrad>(optBase);
    std::vector<float> tmp;
    opt->gt_->get(tmp);
    vGt.insert(vGt.end(), tmp.begin(), tmp.end());
  }

  // save to file
  io::Item item;
  item.name = "adagrad_gt";
  item.shape = Shape({1, (int)vGt.size()});
  item.type = Type::float32;
  item.bytes.resize(vGt.size() * sizeOf(item.type));
  std::copy(
      (char*)vGt.data(), (char*)vGt.data() + vGt.size(), item.bytes.begin());

  io::saveItems(name, {item});
}

void Adagrad::resetStats() {
  if(gt_)
    gt_->set(0.f);
}

// Adam

void Adam::updateImpl(Tensor params, Tensor grads) {
  if(!alloc_)
    alloc_ = New<TensorAllocator>(params->getBackend());

  if(!mt_) {
    int elements = (int)params->size();
    alloc_->reserveExact(2 * params->memory()->size());
    alloc_->allocate(mt_, {1, elements});
    mt_->set(0.f);

    alloc_->allocate(vt_, {1, elements});
    vt_->set(0.f);
  }

  t_++;
  float denom1 = 1 - (float)std::pow(beta1_, t_);
  float denom2 = 1 - (float)std::pow(beta2_, t_);

  using namespace functional;

  Element(_1 = (beta1_ * _1) + ((1 - beta1_) * _2), mt_, grads);
  Element(_1 = (beta2_ * _1) + ((1 - beta2_) * (_2 * _2)), vt_, grads);

  Element(_1 -= (multiplyFactor_ * eta_) * (_2 / denom1)
                / (sqrt(_3 / denom2) + eps_),
          params,
          mt_,
          vt_);

  params->getBackend()->synchronize();
}

void Adam::load(const std::string& name,
                std::vector<Ptr<OptimizerBase>> opts,
                std::vector<Ptr<Backend>> backends) {
  ABORT_IF(opts.size() != backends.size(), "opts and backends of different sizes??");

  if(!boost::filesystem::exists(name))
    return;

  LOG(info, "Loading Adam parameters from {}", name);

  std::vector<float> vMt;
  std::vector<float> vVt;

  auto items = io::loadItems(name);
  for(auto item : items) {
    // get the size of mt_ and vt_, they are the same
    auto totalSize = item.shape.elements();

    // extract data into vectors
    if(item.name == "adam_mt") {
      vMt.resize(totalSize);
      std::copy(
          (float*)item.data(), (float*)item.data() + totalSize, vMt.begin());
    }
    if(item.name == "adam_vt") {
      vVt.resize(totalSize);
      std::copy(
          (float*)item.data(), (float*)item.data() + totalSize, vVt.begin());
    }
  }
  if(vMt.empty() || vVt.empty()) {
    LOG(warn, "[warn] Adam parameters not found in .npz file");
    return;
  }
  ABORT_IF(vMt.size() != vVt.size(), "mt and vt have different sizes??");

  for(size_t id = 0; id < opts.size(); id++) {
    auto opt = std::dynamic_pointer_cast<Adam>(opts[id]);

    size_t totalSize = vMt.size();
    size_t shardSize = (size_t)(ceil(totalSize / (float)opts.size()));
    size_t shift = id * shardSize;
    size_t size = std::min(shardSize, totalSize-shift);

    if(!opt->alloc_)
      opt->alloc_ = New<TensorAllocator>(backends[id]);

    if(!opt->mt_ || !opt->vt_) {
      opt->alloc_->reserveExact(2 * sizeof(float) * size);
      opt->alloc_->allocate(opt->mt_, {1, (int)size});
      opt->alloc_->allocate(opt->vt_, {1, (int)size});
    }

    std::vector<float> tmpMt(vMt.begin() + shift, vMt.begin() + shift + size);
    opt->mt_->set(tmpMt);
    std::vector<float> tmpVt(vVt.begin() + shift, vVt.begin() + shift + size);
    opt->vt_->set(tmpVt);
  }
}

void Adam::save(const std::string& name,
                std::vector<Ptr<OptimizerBase>> opts,
                size_t /*totalSize*/) {
  LOG(info, "Saving Adam parameters to {}", name);

  // fetch and concatenate state vectors from shards into a CPU-side vector
  std::vector<float> vMt;
  for(auto optBase : opts) {
    auto opt = std::dynamic_pointer_cast<Adam>(optBase);
    std::vector<float> tmp;
    opt->mt_->get(tmp);
    vMt.insert(vMt.end(), tmp.begin(), tmp.end());
  }
  std::vector<float> vVt;
  for(auto optBase : opts) {
    auto opt = std::dynamic_pointer_cast<Adam>(optBase);
    std::vector<float> tmp;
    opt->vt_->get(tmp);
    vVt.insert(vVt.end(), tmp.begin(), tmp.end());
  }

  // save to file
  io::Item itemMt;
  itemMt.name = "adam_mt";
  itemMt.shape = Shape({1, (int)vMt.size()});
  itemMt.type = Type::float32;
  itemMt.bytes.resize(vMt.size() * sizeOf(itemMt.type));
  std::copy(
      (char*)vMt.data(), (char*)vMt.data() + vMt.size(), itemMt.bytes.begin());

  io::Item itemVt;
  itemVt.name = "adam_vt";
  itemVt.shape = Shape({1, (int)vVt.size()});
  itemVt.type = Type::float32;
  itemVt.bytes.resize(vVt.size() * sizeOf(itemVt.type));
  std::copy(
      (char*)vVt.data(), (char*)vVt.data() + vVt.size(), itemVt.bytes.begin());

  io::saveItems(name, {itemMt, itemVt});
}

void Adam::resetStats() {
  if(mt_)
    mt_->set(0.f);

  if(vt_)
    vt_->set(0.f);
}

Ptr<OptimizerBase> Optimizer(Ptr<Config> options) {
  float lrate = (float)options->get<double>("learn-rate"); // @TODO: should this be <float>?
  auto params = options->has("optimizer-params")
                    ? options->get<std::vector<float>>("optimizer-params")
                    : std::vector<float>({});

  Ptr<ClipperBase> clipper = nullptr;
  float clipNorm = (float)options->get<double>("clip-norm"); // @TODO: should this be <float>?
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
}  // namespace marian
