#include <ATen/CheckpointTensorImpl.h>
#include <ATen/Logger.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAEvent.h>
#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/core/Allocator.h>


#include <chrono>
#include <string>
#include <random>
#include <cmath>

namespace at {

using Clock = std::chrono::high_resolution_clock;
using Time = Clock::time_point;
using Duration = Clock::duration;
using FinalTime = std::chrono::nanoseconds;

struct PerfStats;

struct Timer {
  std::string name;
  Time start;
  Timer(std::string name, Time start) : name(name), start(start) {}
  Timer() {}
  ~Timer();
};

constexpr bool stats = true;

struct PerfStats {
  using TimerStats = std::tuple<std::string, Time, Time, Duration>;
  Time start;
  std::unordered_map<std::string, int> calls;
  std::vector<PerfStats::TimerStats> timers;

  PerfStats() : start(Clock::now()), calls(0), timers() {}

  /*Timer track(std::string name) {
    if (stats) {
    auto it = this->calls.find(name);
    if (it != this->calls.end()) {
      it->second += 1;
    } else {
      this->calls.insert({name, 0});
    }

    return Timer(name, Clock::now());
    }
    return Timer();
    }*/
  void track(const char*) { }

  ~PerfStats() {
    if (!stats) { return; }
    auto start = std::get<1>(this->timers[0]);
    auto now = Clock::now();
    std::cout << "All done. Here are some perf stats fresh off the preses." << std::endl;
    std::unordered_map<std::string, Duration> durations;

    Duration total = now - this->start;

    // For now simple strategy, count up all the time taken
    // by each "tagged call site".
    for (auto timer : timers) {
      auto name = std::get<0>(timer);
      Duration duration = std::get<3>(timer);
      auto it = durations.find(name);
      if (it != durations.end()) {
        it->second += duration;
      } else {
        durations.insert({name, duration});
      }
    }

    std::vector<std::pair<std::string, Duration>> data;

    // Convert the durations
    for (auto d : durations) {
      // auto duration = std::chrono::duration_cast<FinalTime>(d.second);
      data.push_back(d);
    }

    std::sort(data.begin(), data.end(),
    [](const std::pair<std::string, Duration> & a, const std::pair<std::string, Duration> & b) -> bool {
      return a.second > b.second;
    });

    for (auto d : data) {
      auto duration = std::chrono::duration_cast<FinalTime>(d.second);
      auto total_duration = std::chrono::duration_cast<FinalTime>(total);
      double percentage = ((double)duration.count())/((double)total_duration.count()) * 100;
      auto call_count = this->calls.find(d.first);
      TORCH_CHECK(call_count != this->calls.end());
      std::cout << "CallSite: " << d.first << " CallCount: " << call_count->second << " Cost: " << duration.count() << "ns" << " (%" << percentage << ")" << std::endl;
    }
  }
};

static PerfStats STATS = PerfStats();

size_t memory_sum = 0;
size_t memory_max = 0;
size_t memory_count = 0;

void reset_memory_stat() {
  memory_sum = 0;
  memory_max = 0;
  memory_count = 0;
}

inline size_t memory(const Tensor& t) {
  if (! t.has_storage()) {
    return 0;
  }
  auto& storage = t.storage();
  size_t res = storage.nbytes();
  memory_sum += res;
  memory_max = std::max(memory_max, res);
  memory_count += 1;
  return res;
}

Timer::~Timer() {
  Time now = Clock::now();
  Duration elapsed = now - start;
  PerfStats::TimerStats stats = { name , start, now, elapsed };
  STATS.timers.push_back(stats);
}

bool use_log_ = true;
bool use_profile_ = true;
long base_compute_time_ = 0;
long remat_compute_time_ = 0;
long search_time_ = 0;
long cost_time_ = 0;

long total_count = 0;
long evict_count = 0;
long offload_count = 0;

CheckpointPool pool;
void CheckpointPool::add(const intrusive_ptr<AliasPool>& p) {
  if (p->memory > 0 && (memory_count == 0 || !ignore_small_tensors || p->memory >= 0.01 * double(memory_sum/memory_count))) {
    aps.push_back(weak_intrusive_ptr<AliasPool>(p));
  }
}

long current_memory() {
  STATS.track("current_memory");
  auto device_stat = c10::cuda::CUDACachingAllocator::getDeviceStats(0);
  return device_stat.allocated_bytes[0].current;
}

void CheckpointPool::auto_evict() {
  STATS.track("CheckpointPool::auto_evict");
  if (has_memory_budget) {
    // if (!offload_queue.empty() && current_memory() < memory_budget) {
    //   prefetch();
    // }
    while (current_memory() > memory_budget) {
      // std::cout << "current memory is: " << current_memory() << std::endl;
      evict();
    }
  }
}

void CheckpointPool::evict() {
  time_t pre = std::chrono::system_clock::now();
  STATS.track("CheckpointPool::evict");
  TORCH_CHECK(aps.size() > 0);
  // shrunk: either something has been evicted or the pools have gotten smaller
  bool shrunk = false;
  int evict_idx = -1;
  double filter_score = INFINITY;
  time_t current_time = std::chrono::system_clock::now();
  auto remove_from_aps = [&](size_t i) {
                           aps[i] = aps[aps.size() - 1];
                           aps.pop_back();
                         };
  std::uniform_int_distribution<> distrib(1, 1 * std::max(1, static_cast<int>(std::sqrt(aps.size()))));
  // sampling a random independent subset of all evictable tensors to find the cheapest tensor to evict.
  // std::cout << "aps size: " << aps.size() << std::endl;
  for (size_t i = 0; i < aps.size();) {
    auto cannot_evict = [&]() {
                          shrunk = true;
                          remove_from_aps(i);
                        };
    auto ap_strong = aps[i].lock();
    if (!ap_strong.defined()) {
      cannot_evict();
    }
    else if (ap_strong->ecn) {
      cannot_evict();
    } 
    else if (ap_strong->is_offloaded) {
      cannot_evict();
    } 
    else {
      if (ap_strong->evictable()) {
        double cost = ap_strong->cost(current_time);
        if (cost < filter_score) {
          filter_score = cost;
          evict_idx = i;
        }
      }

      if (sample_tensors) {
        i += distrib(gen);
      } else {
        i += 1;
      }
    }
  }
  // std::cout << "evict idx " << evict_idx << std::endl;
  if (evict_idx == -1) {
    TORCH_CHECK(shrunk);
  } else {
    auto release_aps = aps[evict_idx].lock();
    double real_decision;
    std::cout << "release_aps->swapped " << release_aps->swapped << std::endl;
    if (release_aps->swapped) {
      double _computed_cost = release_aps->compute_cost_func(current_time);
      double _swap_cost = release_aps->swap_cost;
      real_decision = _swap_cost / _computed_cost;
    } else {
      real_decision = release_aps->decision_func(current_time);
    }
    std::cout << "decision is  " << real_decision << std::endl;
    auto evict_from_idx = [&](size_t idx) {
                            auto ap_strong = aps[idx].lock();
                            TORCH_CHECK(ap_strong.defined());
                            ap_strong->evict();
                            remove_from_aps(evict_idx);
                          };
    auto offload_from_idx = [&](size_t idx){
                            auto ap_strong = aps[idx].lock();
                            TORCH_CHECK(ap_strong.defined());
                            ap_strong->offload();
                            remove_from_aps(evict_idx);
                          };
    total_count += 1;
    if (real_decision > 1) {
      evict_from_idx(evict_idx);
      evict_count += 1;
      // std::cout << "evict aaaaa" << std::endl;
      // offload_from_idx(evict_idx);
    } else {
      // evict_from_idx(evict_idx);
      offload_from_idx(evict_idx);
      offload_count += 1;
      // auto ap_strong = aps[evict_idx].lock();
      // TORCH_CHECK(ap_strong.defined());
      // remove_from_aps(evict_idx);
    }
  }
  std::cout << "total count is: " << total_count << std::endl;
  std::cout << "evict count is: " << evict_count << std::endl;
  std::cout << "offload count is: " << offload_count << std::endl; 

  time_t post = std::chrono::system_clock::now();
  search_time_ += (post - pre).count();
}

void CheckpointPool::prefetch() {
  if (offload_queue.empty()) {
    return;
  }
  strong front_operand = offload_queue.front();
  if (front_operand) {
    for (int i=0; i < prefetch_count; i++) {
      auto total_memory = current_memory() + memory(front_operand->get());
      if (total_memory < memory_budget) {
        front_operand->reload();
        offload_queue.pop();
      }
      if (offload_queue.empty()) {
        break;
      } else {
        front_operand = offload_queue.front();
      }
    }
  }
  return; 
}

CheckpointPool::CheckpointPool() { }

namespace native {

Tensor checkpoint(const Tensor& t) {
  STATS.track("checkpoint");
  auto cpti = intrusive_ptr<CheckpointTensorImpl>::make(t);
  if (use_log_) {
    DTRLogConstant(cpti->counter_name());
    DTRLogMemory(cpti->counter_name(), cpti->ref->value->value->memory());
  }
  return Tensor(cpti);
}

Tensor uncheckpoint(const Tensor& t) {
  STATS.track("uncheckpoint");
  auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
  TORCH_CHECK(cpti != nullptr);
  return cpti->ref->value->value->get();
}

void pin(Tensor& t) {
  STATS.track("pin");
  auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
  TORCH_CHECK(cpti != nullptr);
  cpti->ref->value->value->pin();
}

Tensor decheckpoint(const Tensor& t) {
  STATS.track("decheckpoint");
  auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
  return cpti ? cpti->ref->value->value->get() : t;
}

bool is_checkpoint(const Tensor& t) {
  STATS.track("is_checkpoint");
  auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
  return cpti != nullptr;
}

Tensor try_checkpoint(const Tensor& t) {
  STATS.track("try_checkpiont");
  return is_checkpoint(t) ? t : checkpoint(t);
}

void new_log(std::string str) {
  DTRLogger::logger().out = std::ofstream(DTRLogger::logger().get_filename(str));
}

void annotate_log(std::string str) {
  if (!use_log_) { return; }
  if (log_json) {
    json j;
    j[INSTRUCTION] = "ANNOTATE";
    j[ANNOTATION] = str;
    DTRLogger::logger().log(j.dump());
  } else {
    DTRLogger::logger().log("# " + str);
  }
}

void toggle_log(bool b) {
  use_log_ = b;
}

void clear_checkpointpool() {
  while (likely(!pool.exts.empty())) {
    if (auto e = pool.exts.back().lock()) {
      e->value->pin();
    }
    pool.exts.pop_back();
  }
}

void unset_memory_budget() {
  pool.has_memory_budget = false;
}

void set_memory_budget(long budget) {
  std::cout << "memory budget: " << budget << std::endl;
  pool.memory_budget = budget;
  pool.has_memory_budget = true;
}

void toggle_sampling(bool sample) {
  pool.sample_tensors = sample;
}

void toggle_ignore_small_tensors(bool ignore) {
  pool.ignore_small_tensors = ignore;
}

void reset_profile() {
  base_compute_time_ = 0;
  remat_compute_time_ = 0;
  search_time_ = 0;
  cost_time_ = 0;
}

void toggle_profile(bool profile) {
  use_profile_ = profile;
}

long compute_time() {
  return base_compute_time() + remat_compute_time();
}

long cost_time() {
  return cost_time_;
}

long search_time() {
  return search_time_;
}

long remat_compute_time() {
  return remat_compute_time_;
}

long base_compute_time() {
  return base_compute_time_;
}

long loop_time() {
  return search_time() - cost_time();
}

}

void CheckpointTensorCell::reload() {
  STATS.track("AliasPool::reload");
  onGPU = true;
  offloaded = false;
  at::cuda::CUDAStream stream = at::cuda::getCurrentCUDAStream();
  cudaMemcpyKind kind = cudaMemcpyHostToDevice;
  // size_t res = input->cpuStorage->nbytes();
  c10::Allocator* allocator = at::cuda::getCUDADeviceAllocator();
  // auto ptr = std::make_shared<Storage>(Storage::use_byte_size_t(), res, allocator, false);
  // cudaMemcpyAsync(ptr->data_ptr().get(), input->cpuStorage->data_ptr().get(), res, kind, stream);
  c10::Device device = optional_device_.value();
  // std::cout << "device: " << device << std::endl;
  Tensor gpuTensor = cpuTensor.to(device);
  cpuTensor.reset();
  t = std::make_unique<Tensor>(gpuTensor.detach());
  pool->onGPU = true;
  pool->is_offloaded = false;
  at::pool.aps.push_back(weak_intrusive_ptr<AliasPool>(pool));
  // how to set tensor device and on undefined tensor error
  // input->cpuStorage.reset();
  // std::cout << "Reload finished " << std::endl;
  // return gpuTensor;
}

[[inline]]
Tensor uncheckpoint(const strong& input) {
  return input->get();
}

Tensors uncheckpoint(strongs& inputs) {
  STATS.track("uncheckpoint");
  Tensors ret;
  ret.reserve(inputs.size());
  for (strong& input : inputs) {
    // inlined manually
    // if (!input->onGPU && input->offloaded) {
    //   input->reload();
    //   ret.push_back(input->get());
    // }
    // else if (!input->onGPU && input->evicted) {
    //   ret.push_back(input->get());
    // } else {
    //   ret.push_back(input->get());
    // }
    // reload(input);
    ret.push_back(input->get());
  }
  return ret;
};

Tensors try_checkpoint(const Tensors& inputs) {
  STATS.track("try_checkpoint");
  Tensors ret;
  ret.reserve(inputs.size());
  for (const Tensor& input : inputs) {
    ret.push_back(at::native::try_checkpoint(input));
  }
  return ret;
}

CheckpointInfo merge_cpi(CheckpointInfo l, CheckpointInfo r) {
  STATS.track("merge_cpi");
  return CheckpointInfo(l.compute_cost + r.compute_cost);
}

void AliasPool::evict() {
  STATS.track("AliasPool::evict");
  TORCH_CHECK(!ecn);
  ecn = head_remat->get_ecn();
  auto ecns = neighbor_ecn();
  for (const auto& necn : ecns) {
    merge<CheckpointInfo>(merge_cpi, ecn, necn);
  }
  TORCH_CHECK(memory > 0);
  TORCH_CHECK(lock_count == 0);
  TORCH_CHECK(!is_evicted);
  is_evicted = true;
  onGPU = false;
  for (const weak& w : tensors) {
    if (auto cell = w.lock()) {
      cell->evict();
    }
  }
}

void AliasPool::offload() {
  STATS.track("AliasPool::offload");
  // TORCH_CHECK(!ecn);
  // ecn = head_remat->get_ecn();
  // auto ecns = neighbor_ecn();
  // for (const auto& necn : ecns) {
  //   merge<CheckpointInfo>(merge_cpi, ecn, necn);
  // }
  TORCH_CHECK(memory > 0);
  TORCH_CHECK(lock_count == 0);
  TORCH_CHECK(!is_offloaded);
  at::cuda::CUDAStream stream = at::cuda::getCurrentCUDAStream();
  cudaMemcpyKind kind = cudaMemcpyDeviceToHost;
  c10::Allocator* allocator = at::getCPUAllocator();
  onGPU = false;
  is_offloaded = true;
  swapped = true;
  for (const weak& w : tensors) {
    if (auto cell = w.lock()) {
      auto& storage = cell->t->storage();
      size_t res =  storage.nbytes();
      // auto dtype_ = cell->t->dtype();
      time_t pre = std::chrono::system_clock::now();
      // cell->cpuStorage = std::make_shared<Storage>(Storage::use_byte_size_t(), res, allocator, false);
      // cudaMemcpyAsync(cell->cpuStorage->data_ptr().get(), storage.data_ptr().get(), res, kind, stream);
      cell->cpuTensor = cell->t->to(DeviceType::CPU);
      time_t post = std::chrono::system_clock::now();
      cell->t.reset();
      cell->swap_cost = (post - pre).count();
      cell->onGPU = false;
      cell->offloaded = true;
      // std::cout << "offloaded tensor device " << cell->optional_device_.value() << std::endl;
      // pool.offload_queue.push(cell);
    }
  }
  // std::cout << "offload finished  " << std::endl;
}

double AliasPool::cost(time_t current_time) {
  time_t pre = std::chrono::system_clock::now();
  auto cpi = head_remat->get_cpi();
  auto ecns = neighbor_ecn();
  // for (const auto& necn : ecns) {
  //   cpi = merge_cpi(cpi, get_t(necn));
  // }
  auto ret = cpi.cost(memory, (current_time - last_used_time).count());
  time_t post = std::chrono::system_clock::now();
  cost_time_ += (post - pre).count();
  return ret;
}

double AliasPool::compute_cost_func(time_t current_time) {
  time_t pre = std::chrono::system_clock::now();
  auto cpi = head_remat->get_cpi();
  auto ecns = neighbor_ecn();
  for (const auto& necn : ecns) {
    cpi = merge_cpi(cpi, get_t(necn));
  }
  auto ret = cpi.compute_cost_func(memory, (current_time - last_used_time).count());
  time_t post = std::chrono::system_clock::now();
  cost_time_ += (post - pre).count();
  return ret;
}

double AliasPool::decision_func(time_t current_time) {
  time_t pre = std::chrono::system_clock::now();
  auto cpi = head_remat->get_cpi();
  // auto ecns = neighbor_ecn();
  // for (const auto& necn : ecns) {
  //   cpi = merge_cpi(cpi, get_t(necn));
  // }
  auto ret = cpi.fake_decision(memory, (current_time - last_used_time).count());
  time_t post = std::chrono::system_clock::now();
  return ret;
}

void External::release_resources() {
  value->pool->release_external();
  value.reset();
}

void Rematerializer::remat() {
  // TODO: refactor using RAII for exception safety.
  for (const strong& s : inputs) {
    s->pool->lock();
  }
  Tensors ts = uncheckpoint(inputs);
  time_t pre = std::chrono::system_clock::now();
  auto ret = func(ts);
  time_t post = std::chrono::system_clock::now();
  pool.auto_evict();
  remat_compute_time_ += (post - pre).count();
  TORCH_CHECK(ret.size() == outputs.size());
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (auto output_cell = outputs[i].lock()) {
      output_cell->fill(ret[i]);
    }
  }
  ecn.reset();
  for (const strong& s : inputs) {
    s->pool->unlock();
  }
}

ecn_ptr Rematerializer::get_ecn() {
  if (!ecn) {
    ecn = ecn_ptr::make(CheckpointInfo(compute_cost));
  }
  return ecn;
}

CheckpointInfo Rematerializer::get_cpi() {
  return CheckpointInfo(ecn ? duration_t(0) : compute_cost);
}

std::set<ecn_ptr> AliasPool::neighbor_ecn() {
  STATS.track("AliasPool::neighbor_ecn");
  std::set<ecn_ptr> ptr_set;
  int size = neighbors.size();
  for (size_t i = 0; i < size;) {
    if (auto cptc = neighbors[i].lock()) {
      if (cptc->pool->ecn) {
        ptr_set.insert(cptc->pool->ecn);
      }
      ++i;
    } else {
      neighbors[i] = neighbors[size - 1];
      size = size - 1;
    }
  }
  if (size < neighbors.size()) {
    neighbors.erase(neighbors.begin() + size);
  }
  return ptr_set;
}

void AliasPool::set_not_evicted(const intrusive_ptr<AliasPool>& self) {
  if (unlikely(is_evicted)) {
    STATS.track("AliasPool::set_not_evicted(inside)");
    is_evicted = false;
    if (ecn) {
      TORCH_CHECK(head_remat);
      auto cpi = get_t(ecn);
      update_t(ecn, CheckpointInfo(cpi.compute_cost - head_remat->compute_cost));
      ecn.reset();
    }
    pool.add(self);
  }
}

void AliasPool::set_not_offloaded(const intrusive_ptr<AliasPool>& self) {
  if (unlikely(is_offloaded)) {
    STATS.track("AliasPool::set_not_offloaded(inside)");
    is_offloaded = false;
    if (ecn) {
      TORCH_CHECK(head_remat);
      auto cpi = get_t(ecn);
      update_t(ecn, CheckpointInfo(cpi.compute_cost - head_remat->compute_cost));
      ecn.reset();
    }
    pool.add(self);
  }
}

void CheckpointTensorCell::fill(const Tensor& t) {
  STATS.track("CheckpointTensorCell::fill");
  if (!(this->t)) {
    this->t = std::make_unique<Tensor>(t.detach());
    pool->set_not_evicted(pool);
    if (!defined) {
      defined = true;
      is_undefined_tensor = !t.defined();
      key_set_ = t.key_set();
      if (t.requires_grad()) {
        key_set_ = key_set_.add(DispatchKey::Autograd);
      }
      dtype_ = t.dtype();
      optional_device_ = t.optional_device();
    }
  }
}

intrusive_ptr<TensorImpl> CheckpointTensorImpl::shallow_copy_and_detach(const VariableVersion& version_counter,
                                                                        bool allow_tensor_metadata_change) const {
  auto ret = intrusive_ptr<CheckpointTensorImpl>::make(ref);
  if (use_log_) {
    DTRLogCopy(ret->counter_name(), counter_name());
  }
  return ret;
}

void CheckpointTensorImpl::shallow_copy_from(const c10::intrusive_ptr<TensorImpl>& impl) {
  STATS.track("CheckpointTensorCell::shallow_copy_from");
  TORCH_CHECK(impl->key_set().has(DispatchKey::Checkpoint));
  auto* cpti = dynamic_cast<CheckpointTensorImpl*>(impl.get());
  TORCH_CHECK(cpti != nullptr);
  ref->value = cpti->ref->value;
  if (use_log_) {
    DTRLogCopyFrom(counter_name(), cpti->counter_name());
  }
}

int CheckpointTensorImpl::counter = 0;

bool is_alias(const Tensor& l, const Tensor& r) {
  return l.defined() && r.defined() && l.is_alias_of(r);
}

// return an index for alias.
// we dont care which one because they all lead to the same alias pool.
// return -1 for no alias.
// may god forgive my sin.
int get_alias(const Tensors& ts, const Tensor& t) {
  if (t.defined()) {
    for (size_t i = 0; i < ts.size(); ++i) {
      if (ts[i].defined() && t.is_alias_of(ts[i])) {
        return i;
      }
    }
  }
  return -1;
}

struct MakeRawResult {
  std::vector<intrusive_ptr<External>> outputs;
  std::vector<int> aliases;
  duration_t time;
  intrusive_ptr<Rematerializer> rematerializer;
};

void add_neighbor(const strong& l, const strong& r) {
  l->pool->neighbors.push_back(weak(r));
  r->pool->neighbors.push_back(weak(l));
}

// remat take a single vector of tensors,
// while there are two vector, one storing nonconstants and one storing constants.
// the constants are small and they will not be considered for eviction.
// however, we have to stitch the two vectors together to pass it in remat.
// the size_t in constants decide the location to stitch them in, while input_values fill in the rest.
MakeRawResult make_raw(const rematerialize_function_t& remat_f,
                      strongs& inputs) {
  STATS.track("make_raw");
  for (const strong& s : inputs) {
    s->pool->lock();
  }
  Tensors raw_inputs = uncheckpoint(inputs);
  time_t pre = std::chrono::system_clock::now();
  auto raw_outputs = remat_f(raw_inputs);

  time_t post = std::chrono::system_clock::now();
  pool.auto_evict();
  base_compute_time_ += (post - pre).count();
  std::vector<intrusive_ptr<External>> outputs;
  std::vector<int> aliases;
  weaks weak_outputs;
  auto remat = intrusive_ptr<Rematerializer>::make(Unsafe(), remat_f, inputs, post - pre);

  for (const Tensor& t : raw_outputs) {
    intrusive_ptr<AliasPool> alias_pool;
    int alias = get_alias(raw_inputs, t);
    if (alias == -1) {
      auto m = memory(t);
      alias_pool = intrusive_ptr<AliasPool>::make(Unsafe(), remat, m);
      pool.add(alias_pool);
    }
    else {
      alias_pool = inputs[alias]->pool;
      if (alias_pool->head_remat) {
        alias_pool->head_remat->compute_cost += (post - pre);
      }
    }
    auto e = intrusive_ptr<External>::make(t, alias_pool, remat);
    pool.exts.push_back(weak_intrusive_ptr<External>(e));
    alias_pool->tensors.push_back(weak(e->value));
    outputs.push_back(e);
    aliases.push_back(alias);
    weak_outputs.push_back(weak(outputs.back()->value));
  }
  remat->outputs = weak_outputs;
  for (size_t i = 0; i < inputs.size(); ++i) {
    for (size_t j = 0; j < outputs.size(); ++j) {
      if (!is_alias(raw_inputs[i], raw_outputs[j])) {
        add_neighbor(inputs[i], outputs[j]->value);
      }
    }
  }
  for (const strong& s : inputs) {
    s->pool->unlock();
  }
  return {outputs, aliases, post - pre, remat};
}

std::string from_time(duration_t t) {
  return std::to_string(std::chrono::nanoseconds(t).count());
}

Tensors CheckpointTensorImpl::make(const std::string& name,
                                   const rematerialize_function_t& remat,
                                   const Tensors& inputs) {
  STATS.track("CheckPointTensorImpl::make");
  Tensors checkpointed_inputs = try_checkpoint(inputs);
  auto input_size = checkpointed_inputs.size();

  strongs input_values;
  input_values.reserve(input_size);

  std::vector<std::string> args;
  args.reserve(input_size);

  for (const Tensor& t: checkpointed_inputs) {
    auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
    TORCH_CHECK(cpti);
    input_values.push_back(cpti->ref->value->value);
    if (use_log_) {
      args.push_back(cpti->counter_name());
    }
  }

  auto ret = make_raw(remat, input_values);
  Tensors tensors;
  tensors.reserve(ret.outputs.size());

  for (const auto& t: ret.outputs) {
    auto cp = Tensor(intrusive_ptr<CheckpointTensorImpl>::make(t));
    tensors.push_back(cp);
  }

  if (use_log_) {
    std::vector<std::string> res;
    res.reserve(ret.outputs.size());

    for (const auto& tensor : tensors) {
      res.push_back(get_cpti(tensor)->counter_name());
    }

    DTRLogCall(res, name, args, from_time(ret.time));
    for (size_t i = 0; i < tensors.size(); ++i) {
      Tensor t = tensors[i];
      auto cpti = get_cpti(t);
      DTRLogMemory(cpti->counter_name(), cpti->ref->value->value->memory());
      DTRLogAlias(cpti->counter_name(), ret.aliases[i]);
    }
  }

  return tensors;
}

// TODO: check that mutated value does not have alias.
void CheckpointTensorImpl::mutate(const std::string& name,
                                  const mutate_function_t& mutate,
                                  const Tensors& inputs,
                                  const std::vector<size_t>& mutate_idx) {
  auto remat = [=](const Tensors& t) -> Tensors {
                 Tensors new_input_values = t;
                 for (size_t idx: mutate_idx) {
                   new_input_values[idx] = t[idx].clone();
                 }
                 mutate(new_input_values);
                 return new_input_values;
               };
  Tensors checkpointed_inputs = try_checkpoint(inputs);
  strongs input_values;
  std::vector<std::string> args;
  for (const Tensor& t: checkpointed_inputs) {
    auto* cpti = dynamic_cast<CheckpointTensorImpl*>(t.unsafeGetTensorImpl());
    TORCH_CHECK(cpti);
    input_values.push_back(cpti->ref->value->value);
    if (use_log_) {
      args.push_back(cpti->counter_name());
    }
  }
  auto ret = make_raw(remat, input_values);
  const auto& modified = ret.outputs;
  for (size_t idx: mutate_idx) {
    cell_from_tensor(inputs[idx])->value = modified[idx];
  }
  if (use_log_) {
    DTRLogMutate(name, args, mutate_idx, from_time(ret.time));
  }
}

void CheckpointTensorImpl::release_resources() {
  if (use_log_) {
    DTRLogRelease(counter_name());
  }
  ref.reset();
}

CheckpointTensorImpl::CheckpointTensorImpl(const Tensor& t) : CheckpointTensorImpl(intrusive_ptr<External>::make(t)) {
  pool.exts.push_back(weak_intrusive_ptr<External>(ref->value));
}

}