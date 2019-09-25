/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ThreadPool.h>

#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/framework/variable.h"
#include "paddle/fluid/operators/distributed/rpc_common.h"
#include "paddle/fluid/operators/math/math_function.h"
#include "paddle/fluid/operators/math/selected_rows_functor.h"
#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/place.h"

namespace paddle {
namespace operators {
namespace distributed {

using Scope = framework::Scope;
using Variable = framework::Variable;

template <typename T>
class BlockingQueue {
 public:
  explicit BlockingQueue(size_t capacity) : capacity_(capacity) {
    PADDLE_ENFORCE_GT(capacity_, 0, "The capacity must be greater than 0.");
  }

  bool Push(const T& elem) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return queue_.size() < capacity_; });
      PADDLE_ENFORCE_LT(queue_.size(), capacity_);
      queue_.push_back(elem);
    }
    cv_.notify_one();
    return true;
  }

  bool Push(T&& elem) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return queue_.size() < capacity_; });
      PADDLE_ENFORCE_LT(queue_.size(), capacity_);
      queue_.emplace_back(std::move(elem));
    }
    cv_.notify_one();
    return true;
  }

  T Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [=] { return !queue_.empty(); });
    T rc(std::move(queue_.front()));
    queue_.pop_front();
    cv_.notify_one();
    return rc;
  }

  size_t Cap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  const size_t capacity_;
  std::deque<T> queue_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

template <typename T, int MajorType = Eigen::RowMajor,
          typename IndexType = Eigen::DenseIndex>
using EigenVector = framework::EigenVector<T, MajorType, IndexType>;

inline void MergeVars(const std::string& var_name,
                      const std::vector<std::shared_ptr<Variable>>& vars,
                      Scope* scope) {
  PADDLE_ENFORCE(!vars.empty(), "should have value to merge!");
  auto cpu_place = platform::CPUPlace();
  auto& var0 = vars[0];
  auto* out_var = scope->Var(var_name);
  if (var0->IsType<framework::LoDTensor>()) {
    auto dims = var0->Get<framework::LoDTensor>().dims();
    VLOG(3) << "merge " << var_name << " LoDTensor dims " << dims;

    // init output tensor
    auto* out_t = out_var->GetMutable<framework::LoDTensor>();
    out_t->mutable_data<float>(dims, cpu_place);

    // check the input dims
    for (auto& var : vars) {
      auto& var_t = var->Get<framework::LoDTensor>();
      PADDLE_ENFORCE_EQ(var_t.dims(), dims, "should have the same dims");
    }

    // set output tensor to 0.
    auto cpu_ctx = paddle::platform::CPUDeviceContext();
    math::SetConstant<paddle::platform::CPUDeviceContext, float>
        constant_functor;
    constant_functor(cpu_ctx, out_t, static_cast<float>(0));

    // sum all vars to out
    auto result = EigenVector<float>::Flatten(*out_t);
    for (auto& var : vars) {
      auto& in_t = var->Get<framework::LoDTensor>();
      auto in = EigenVector<float>::Flatten(in_t);
      result.device(*cpu_ctx.eigen_device()) = result + in;
    }
    result.device(*cpu_ctx.eigen_device()) =
        result / static_cast<float>(vars.size());
  } else if (var0->IsType<framework::SelectedRows>()) {
    auto& slr0 = var0->Get<framework::SelectedRows>();
    auto* out_slr = out_var->GetMutable<framework::SelectedRows>();
    out_slr->mutable_rows()->clear();
    out_slr->mutable_value()->mutable_data<float>({{}}, cpu_place);
    std::vector<const paddle::framework::SelectedRows*> inputs;
    inputs.reserve(vars.size());
    for (auto& var : vars) {
      inputs.push_back(&var->Get<framework::SelectedRows>());
    }
    auto dev_ctx = paddle::platform::CPUDeviceContext();
    math::scatter::MergeAverage<paddle::platform::CPUDeviceContext, float>
        merge_average;
    merge_average(dev_ctx, inputs, out_slr);
    VLOG(3) << "merge " << var_name << " SelectedRows height: " << slr0.height()
            << " dims: " << slr0.value().dims();
  } else {
    PADDLE_THROW("unsupported var type!");
  }
}

using RpcCtxMap = std::unordered_map<std::string, RpcContext>;

class Communicator {
 public:
  Communicator() {}
  virtual ~Communicator() {}

  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual bool IsRunning() { return running_; }

  virtual void Send(const std::string& var_name,
                    const framework::Scope& scope) = 0;
  virtual void Recv() = 0;

  virtual void InitImpl(const RpcCtxMap& send_varname_to_ctx,
                        const RpcCtxMap& recv_varname_to_ctx,
                        Scope* recv_scope) = 0;

  virtual void InitImpl(const paddle::framework::ProgramDesc& program,
                        Scope* recv_scope) = 0;

  // for geo-sgd
  virtual void InitImpl(const paddle::framework::ProgramDesc& program, 
                        Scope* param_scope,
                        std::map<std::string,std::map<std::string,std::vector<std::string>>> &vars_info,
                        int &trainers, 
                        int &geo_need_push_nums) = 0;

  virtual void InitImpl(const RpcCtxMap& send_varname_to_ctx, 
                        const RpcCtxMap& recv_varname_to_ctx,
                        Scope* training_scope,
                        int &trainers, 
                        int &geo_need_push_nums,
                        std::unordered_map<std::string,bool> &var_list) = 0;

  static Communicator* GetInstance() { return communicator_.get(); }

  static std::shared_ptr<Communicator> GetInstantcePtr() {
    return communicator_;
  }

  template <typename T>
  static Communicator* InitInstance(const RpcCtxMap& send_varname_to_ctx,
                                    const RpcCtxMap& recv_varname_to_ctx,
                                    Scope* recv_scope) {
    std::call_once(init_flag_, &Communicator::InitWithRpcCtx<T>,
                   send_varname_to_ctx, recv_varname_to_ctx, recv_scope);
    return communicator_.get();
  }

  // Init is called by InitInstance.
  template <typename T>
  static void InitWithRpcCtx(const RpcCtxMap& send_varname_to_ctx,
                             const RpcCtxMap& recv_varname_to_ctx,
                             Scope* recv_scope) {
    if (communicator_.get() == nullptr) {
      communicator_.reset(new T());
      communicator_->InitImpl(send_varname_to_ctx, recv_varname_to_ctx,
                              recv_scope);
    }
  }

  template <typename T>
  static Communicator* InitInstance(
      const paddle::framework::ProgramDesc& program, Scope* recv_scope) {
    std::call_once(init_flag_, &Communicator::InitWithProgram<T>, program,
                   recv_scope);
    return communicator_.get();
  }

  template <typename T>
  static Communicator* InitInstance(const paddle::framework::ProgramDesc& program, 
                                    Scope* training_scope,
                                    std::map<std::string,std::map<std::string,std::vector<std::string>>> vars_info,
                                    int &trainers,
                                    int &geo_need_push_nums) {
    std::call_once(init_flag_, &Communicator::InitWithTranspilerInfo<T>, 
                   program, training_scope, vars_info, trainers, geo_need_push_nums);
    return communicator_.get();
  }

  template <typename T>
  static void InitWithProgram(const paddle::framework::ProgramDesc& program,
                              Scope* recv_scope) {
    if (communicator_.get() == nullptr) {
      communicator_.reset(new T());
      communicator_->InitImpl(program, recv_scope);
    }
  }

  template <typename T>
  static void InitWithTranspilerInfo(const paddle::framework::ProgramDesc& program, 
                                     Scope* training_scope,
                                     std::map<std::string,std::map<std::string,std::vector<std::string>>> &vars_info,
                                     int &trainers,
                                     int &geo_need_push_nums) {
    if (communicator_.get() == nullptr) {
      communicator_.reset(new T());
      communicator_->InitImpl(program, training_scope, vars_info, trainers, geo_need_push_nums);
    }
  }

 protected:
  bool running_ = false;
  static std::shared_ptr<Communicator> communicator_;
  static std::once_flag init_flag_;
};

using SparseIdsMap = std::unordered_map<std::string,std::unordered_set<int64_t>>;

class AsyncCommunicator : public Communicator {
 public:
  AsyncCommunicator() {}
  ~AsyncCommunicator();
  void Start() override;
  void Stop() override;

  void Send(const std::string& var_name,
            const framework::Scope& scope) override;
  void Recv() override;
  void RecvAll();

  void InitImpl(const RpcCtxMap& send_varname_to_ctx,
                const RpcCtxMap& recv_varname_to_ctx,
                Scope* recv_scope) override;

  void InitImpl(const paddle::framework::ProgramDesc& program,
                Scope* recv_scope) override;

  void SendThread();
  void RecvThread();

 private:
  std::unordered_map<std::string,
                     std::shared_ptr<BlockingQueue<std::shared_ptr<Variable>>>>
      send_varname_to_queue_;
  RpcCtxMap send_varname_to_ctx_;
  RpcCtxMap recv_varname_to_ctx_;
  std::unique_ptr<std::thread> send_thread_{nullptr};
  std::unique_ptr<std::thread> recv_thread_{nullptr};
  Scope* recv_scope_;                  // should be global scope
  std::unique_ptr<Scope> send_scope_;  // an independent scope
  std::unique_ptr<::ThreadPool> send_threadpool_{nullptr};
  std::unique_ptr<::ThreadPool> recv_threadpool_{nullptr};
  std::atomic_uint grad_num_{0};  // the num of gradient sent since last recv
};

class GeoSgdCommunicator : public Communicator {
 public:
  GeoSgdCommunicator(){}
  ~GeoSgdCommunicator();
  void InitImpl(const paddle::framework::ProgramDesc& program, 
                Scope* param_scope,
                std::map<std::string,std::map<std::string,std::vector<std::string>>> &vars_info,
                int &trainers, 
                int &geo_need_push_nums) override;

  void InitImpl(const RpcCtxMap& send_varname_to_ctx, 
                const RpcCtxMap& recv_varname_to_ctx,
                Scope* training_scope,
                int &trainers, 
                int &geo_need_push_nums,
                std::unordered_map<std::string,bool> &var_list) override;

  void Start() override;
  void Stop() override;


  //void Send() override;
  void Recv() override;
  void RecvAll();
  
  
 private:
  void GeoSgdStart(const std::string& var_name, const framework::Scope& scope);
  std::unordered_set<int64_t> SparseIdsMerge(std::vector<SparseIdsMap> &ids_send_vec, 
                                             const std::string &var_name);
  
  void SendUpdateDenseVars(const std::string& var_name);
  void SendUpdateSparseVars(const std::string& var_name,std::unordered_set<int64_t> &ids_table);
  void RecvUpdateVars(const std::string& var_name);

  void GeoSgdParamInit(framework::Scope *scope) {
    for(auto &iter:var_list_){
      auto var_name = iter.first;
      scope->Var(var_name);
    }
  }

  void GeoSgdParamCopy(const framework::Scope &scope_x,
                       const framework::Scope &scope_y,
                       const std::string var_name);

  void GeoSgdSparseParamInit(const framework::Scope &scope_x,
                             const framework::Scope &scope_y,
                             const std::string var_name);

  const std::string VarToDeltaVar(const std::string var_name) {
    std::string delta_name = var_name;
    const std::string send_name = delta_name.append(".delta");
    return send_name;
  }

  const std::string DeltaVarToVar(const std::string var_name) {
    std::string origin_name = var_name;
    origin_name.erase(origin_name.find(".delta"),6);
    const std::string param_name = origin_name;
    return param_name;
  }

 private:
  int trainer_nums_ = 1;
  int geo_need_push_nums_ = 100;
  bool is_geo_sgd_ = false;
  Scope *training_scope_;
  std::shared_ptr<Scope> delta_scope_; //parameter local delta: recv - old
  std::shared_ptr<Scope> old_scope_; //parameter local, storage the param after last recv
  std::shared_ptr<Scope> pserver_scope_; //parameter on pserver,gloabl scope

  std::atomic_uint have_push_{0};
  std::unordered_map<std::string,bool> var_list_; //if var is sparse, using selected rows, bool=true
  
  std::shared_ptr<BlockingQueue<std::shared_ptr<SparseIdsMap>>> need_push_queue_;
  std::vector<SparseIdsMap> ids_send_vec_;
};

}  // namespace distributed
}  // namespace operators
}  // namespace paddle