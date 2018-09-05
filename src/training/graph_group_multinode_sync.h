#pragma once

#include "training/graph_group.h"
#include "training/communicator.h"

#ifdef CUDA_FOUND
#include "cuda_runtime.h"
#endif

#include <condition_variable>
#include <future>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "3rd_party/threadpool.h"

namespace marian {

/**
 * Multi-node graph group for asynchronous training over multiple
 * machines each with one or multiple GPUs
 */
class MultiNodeGraphGroupSync : public GraphGroup {
public:
  virtual void setScheduler(Ptr<Scheduler> scheduler) override;

private:
  ////////////////////////////////////////////////////////////////////////////
  // General variables.
  /** Number of clients on nodes in MPI world (cluster). */
  std::vector<int> numberClientsOfNodes_;  //@TODO not used for now, but might
                                           // be useful maybe?

  /** Whether graph group has been properly initialized with a first batch. */
  bool initialized_{false};

  /** Memory allocators for tensors (GPUs). */
  std::vector<Ptr<TensorAllocator>> allocators_;

  ////////////////////////////////////////////////////////////////////////////
  // Client variables.

  /** Graph builders for clients (which run forward and backward passes). */
  std::vector<Ptr<models::ModelBase>> clientBuilders_;

  /** Graphs of clients. One entry per GPU on this node. */
  std::vector<Ptr<ExpressionGraph>> clientGraphs_; // [num local GPUs]

  /** Devices (GPUs) on this node. */
  std::vector<size_t> devices_; // [num local GPUs]

  /** Mutex to ensure clients are uniquely assigned to graphs and builders. */
  std::mutex mutexClientInit_;

  /** Mutex to avoid race conditions in scheduler. */
  std::mutex schedulerMutex_;

  /**
   * Global batch counter used for evenly distributing mini-batches across
   * nodes.
   * Global means that on all workers, this batch id refers to the same batch,
   * while each worker only processes a subset of batches.
   * Nodes process batches round-robin. Specifically, each node processes
   * the subset of batches with batchIter_ % mpi_->commWorldSize() == mpi_->myRank()).
   * @TODO: This is bad. The batches should be global and split into sub-batches across nodes.
   *        Otherwise batch ids are not comparable.
   */
  size_t batchIter_ = 0;

  ////////////////////////////////////////////////////////////////////////////
  // Communication variables.

  /**
   * Variables for optimizer delay and synchronous SGD
   */
  size_t tau_{1};
  std::mutex sumGradientMutex_;
  std::mutex updateParamsMutex_;
  std::mutex sumCostMutex_;
  Tensor accGradientsSync;
  Tensor sumGradientBuffer;
  Tensor paramsAvg_;
  std::vector<float> accGradientsSync_cpu;
  std::vector<float> receiveBuffer_cpu;
  bool synchronization_happened{false};

  Ptr<OptimizerBase> syncOptimizer_;

  std::vector<std::mutex> optDelayMutex_;
  std::vector<size_t> delay_count;
  std::vector<int> totalBatchWords;
  std::vector<Tensor> accGradients, accGradientBuffer;

  bool movingAvg_{false};
  float mvDecay_{1e-4f};

  /**
   * Allocate new tensor on given GPU and store allocator.
   */
  Tensor newTensor(int size, Ptr<Backend> backend);

  /*
   * exponential smoothing
   */
  void updateAvgParams(Tensor paramsAvg, Tensor params, size_t batches);

  /**
   * Setup training environment and launch server thread and (if enabled) client
   * communication overlap threads..
   * Includes setting up MPI, node and shard sizes, clients, server shards and
   * communication overlap stuff.
   */
  virtual void init(Ptr<data::Batch> batch);

  /**
   * Setup clients that will compute gradients and communicate them with the
   * server shards.
   * There is one client per GPU.
   */
  void setupClients(Ptr<data::Batch> batch);

  /**
   * Initialize the graphs (models) of all clients on this node with the given
   * batch.
   */
  void runBatchThroughClientGraphs(Ptr<data::Batch> batch);

  /**
   * Initialize the CPU arrays, with pinned memory for faster CudaMemCpy
   * operations.
   */
  void initCPUArrays();

  /**
   * Sums the gradients from a node, taking care of locking
   * @param gradient - the gradient
   */

  void sumGRAD(Tensor gradient);

  /**
   * Does the MPI Communication, parameter update and copying back parameters.
   * @TODO ALHAM. God function too godly?
   */
  void sendReceiveUpdateSync();

  void execute(Ptr<data::Batch> batch);

  /**
   * Load the GPU configuration of this node (i.e. which GPUs to use) and the
   * number of GPUs on the other nodes.
   * Specifically, this sets up numberClientsOfNodes_[]. It does not communicate with other nodes.
   */
  void loadDeviceConfig(std::vector<size_t> deviceConfig) { // deviceConfig = array of GPU device ids for this worker --@TODO: rename to deviceIds, or just devices?
    size_t index = 0, node = 0, nClientsSeen = 0;
    numberClientsOfNodes_ = std::vector<int>(mpi_->commWorldSize(), 0); // @TODO: use assign(n, 0)
    // @TODO: What does this logic do??
    while(index < deviceConfig.size()) {
      if(numberClientsOfNodes_[node] == 0) {
        numberClientsOfNodes_[node] = (int)deviceConfig[index];
        nClientsSeen = 0;
      } else if(nClientsSeen < numberClientsOfNodes_[node]) {
        if(node == mpi_->myRank()) {
          devices_.push_back(deviceConfig[index]);
        }
        nClientsSeen++;
      } else {
        node++;
        index--;
      }
      index++;
    }
  }

public:
  /**
   * (Constructor) Call super class and initialize client graphs and builders.
   */
  MultiNodeGraphGroupSync(Ptr<Config> options)
      : GraphGroup(options),
        tau_{options_->get<size_t>("optimizer-delay")},
        movingAvg_{options_->get<float>("exponential-smoothing") > 0}, // @TODO: redundant
        mvDecay_{options_->get<float>("exponential-smoothing")},
        syncOptimizer_{Optimizer(options_)} { // @BUGBUG? Do we really have two optimizers?
    setupMPI();  // Setup MPI first thing

    // Set up devices for this node
    std::vector<size_t> devices; // set of GPU device ids for this worker
    for(auto& d : options_->getDevices())
      devices.push_back(d.no);
    loadDeviceConfig(devices); // set up numberClientsOfNodes_[]  --@TODO: not clear what it is for, or even what it is

    // Create builders and graphs for clients; that is, for each GPU we use on this node.
    for(size_t i = 0; i < devices_.size(); i++) {
      clientGraphs_.push_back(New<ExpressionGraph>());
      clientGraphs_[i]->setDevice({devices_[i], DeviceType::gpu});
      clientGraphs_[i]->reserveWorkspaceMB(options_->get<size_t>("workspace"));
      clientBuilders_.push_back(
          models::from_config(options_, models::usage::training));
    }
  }

  /**
   * Update any client model with given batch if batch is assigned to this node.
   */
  void update(Ptr<data::Batch> batch) override {
    ABORT_IF(finalized_, "Training has already finished.");
    if(batchIter_ % mpi_->commWorldSize()
       == mpi_->myRank()) {  // Only take batch assigned to this node
      execute(batch);
    }
    batchIter_++;
  }

  /**
   * Load models from disk if file exists and setting is not disabled
   * @TODO: How is this specific to multi-node? This a general operation, no? Code dup
   */
  void load() override {
    if(!options_->get<bool>("no-reload")) {
      std::string name = options_->get<std::string>("model");

      if(boost::filesystem::exists(name)) {
        if(scheduler_)
          scheduler_->load(name);
        size_t i = 0;
        for(auto graph : clientGraphs_)
          clientBuilders_[i++]->load(graph, name);
      } else if(options_->has("pretrained-model")) {
        std::string init = options_->get<std::string>("pretrained-model");
        LOG(info,
            "Initialize model weights with the pre-trained model {}",
            init);
        size_t i = 0;
        for(auto graph : clientGraphs_)
          clientBuilders_[i++]->load(graph, init, false);
      }
    }
  }

  /**
   * Save model of first client's graph to disk
   * @BUGBUG: Only node[0] should save the model, no? Or are these assumed to be local directories?
   */
  void save(bool final = false) override { save(clientGraphs_[0], final); }

  /**
   * Save model of given graph to disk.
   */
  void save(Ptr<ExpressionGraph> graph, bool final = false) {
    // recover which client (device) owns this graph
    int idx = 0;
    for(int i = 0; i < clientGraphs_.size(); ++i) {
      if(graph == clientGraphs_[i]) {
        idx = i;
        break;
      }
    }

    // @TODO: This code does not seem specific to multi-node. Remove code dup.
    if(options_->get<bool>("overwrite")) {
      std::string name = options_->get<std::string>("model");

      clientBuilders_[idx]->save(clientGraphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    } else {
      std::string name = options_->get<std::string>("model");

      if(!final) {
        std::string numberOfBatches
            = scheduler_ ? std::to_string(scheduler_->numberOfBatches())
                         : "unknown";
        std::string nameOverwrite = name;
        nameOverwrite.replace(
            name.size() - 4, 4, ".iter" + numberOfBatches + ".npz");
        clientBuilders_[idx]->save(clientGraphs_[idx], nameOverwrite);
      }

      clientBuilders_[idx]->save(clientGraphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    }
  }

  /**
   * Collect statistics from first client's graph.
   * @BUGBUG: This assumes that all GPUs in the worker are the same, but not across workers. Meaningful? Better determine this once and broadcast.
   */
  Ptr<data::BatchStats> collectStats() {
    return GraphGroup::collectStats(
        clientGraphs_[0], clientBuilders_[0], devices_.size());
  }
};
}  // namespace marian
