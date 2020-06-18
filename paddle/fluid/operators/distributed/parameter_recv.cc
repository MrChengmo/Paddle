//   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "paddle/fluid/operators/distributed/parameter_recv.h"

#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/framework/selected_rows.h"
#include "paddle/fluid/framework/tensor.h"

#include "paddle/fluid/operators/distributed/distributed.h"
#include "paddle/fluid/operators/distributed/rpc_client.h"
#include "paddle/fluid/operators/distributed/variable_response.h"
#include "paddle/fluid/operators/distributed_ops/send_recv_util.h"
#include "paddle/fluid/operators/strided_memcpy.h"

namespace paddle {
namespace operators {
namespace distributed {

using LoDTensor = framework::LoDTensor;
using LoDTensor = framework::LoDTensor;
using SelectedRows = framework::SelectedRows;
using DDim = framework::DDim;

template <typename T>
void ParameterRecv<T>::operator()(const CommContext &rpc_ctx,
                                  const framework::Scope &scope, bool barrier) {
  VLOG(2) << "ParameterRecv in " << rpc_ctx.var_name;

  PADDLE_ENFORCE_GE(rpc_ctx.origin_varnames.size(), 1,
                    platform::errors::InvalidArgument(
                        "origin_varnames.size() >= 1 is permitted"));

  auto *origin_var = scope.FindVar(rpc_ctx.origin_varnames[0]);

  if (origin_var->IsType<framework::SelectedRows>()) {
    PADDLE_THROW(
        platform::errors::InvalidArgument("Only support LodTensor now"));
  }

  platform::DeviceContextPool &pool = platform::DeviceContextPool::Instance();
  auto cpu_place = platform::CPUPlace();
  auto &cpu_ctx = *pool.Get(cpu_place);

  distributed::RPCClient *rpc_client =
      distributed::RPCClient::GetInstance<RPCCLIENT_T>(rpc_ctx.trainer_id);

  std::vector<distributed::VarHandlePtr> rets;

  // variable do not spilt
  if (rpc_ctx.origin_varnames.size() == 1 &&
      rpc_ctx.splited_varnames.size() == 1) {
    auto varname = rpc_ctx.origin_varnames[0];
    auto *var = scope.FindVar(varname);
    VLOG(4) << "recv " << varname << " from " << rpc_ctx.epmap[0];
    rets.push_back(rpc_client->AsyncGetVarNoBarrier(rpc_ctx.epmap[0], cpu_ctx,
                                                    scope, varname, varname));

    for (size_t i = 0; i < rets.size(); i++) {
      PADDLE_ENFORCE_NE(
          rets[i]->Wait(), 0U,
          platform::errors::ExecutionTimeout("internal error in RPCClient"));
    }

    VLOG(3) << "ParameterRecv out " << rpc_ctx.var_name;
    return;
  }

  std::unique_ptr<framework::Scope> local_scope = scope.NewTmpScope();

  if (barrier) {
    for (size_t i = 0; i < rpc_ctx.splited_varnames.size(); i++) {
      auto &recv_var_name = rpc_ctx.splited_varnames[i];
      local_scope->Var(recv_var_name);
      VLOG(4) << "recv " << recv_var_name << " from " << rpc_ctx.epmap[i];
      // sparse param in recv_scope is LoDTensor
      rets.push_back(rpc_client->AsyncGetVar(rpc_ctx.epmap[i], cpu_ctx,
                                             *local_scope.get(), recv_var_name,
                                             recv_var_name));
    }
  } else {
    for (size_t i = 0; i < rpc_ctx.splited_varnames.size(); i++) {
      auto &recv_var_name = rpc_ctx.splited_varnames[i];
      local_scope->Var(recv_var_name);
      VLOG(4) << "recv " << recv_var_name << " from " << rpc_ctx.epmap[i];
      // sparse param in recv_scope is LoDTensor
      rets.push_back(rpc_client->AsyncGetVarNoBarrier(
          rpc_ctx.epmap[i], cpu_ctx, *local_scope.get(), recv_var_name,
          recv_var_name));
    }
  }

  for (size_t i = 0; i < rets.size(); i++) {
    PADDLE_ENFORCE_NE(rets[i]->Wait(), 0U, platform::errors::ExecutionTimeout(
                                               "internal error in RPCClient"));
  }

  std::vector<Variable *> variables;
  for (auto &slice_varname : rpc_ctx.splited_varnames) {
    Variable *var = local_scope->FindVar(slice_varname);

    PADDLE_ENFORCE_EQ(
        var->IsInitialized(), true,
        platform::errors::InvalidArgument("recv var should be inited"));

    variables.push_back(var);
  }

  // merged var's tensor into one
  auto merged_var = std::make_shared<Variable>();

  framework::FlattenVariable(variables, merged_var.get());
  auto src_ptr = merged_var->Get<framework::LoDTensor>().data<void>();
  // write tensor to global scope
  for (auto &origin_varname : rpc_ctx.origin_varnames) {
    Variable *origin_v = scope.FindVar(origin_varname);
    framework::Tensor *origin_t = origin_v->GetMutable<framework::LoDTensor>();
    auto size = origin_t->numel() * framework::SizeOfType(origin_t->type());
    memory::Copy(cpu_place, origin_t->data<void>(), cpu_place, src_ptr, size);
    src_ptr = reinterpret_cast<char *>(const_cast<void *>(src_ptr)) + size;
  }
  VLOG(3) << "ParameterRecv out " << rpc_ctx.var_name;
}

template <typename T>
void ParameterRecv<T>::operator()(const CommContext &rpc_ctx,
                                  const framework::Scope &scope) {
  this->operator()(rpc_ctx, scope, true);
}

template struct ParameterRecv<float>;

};  // namespace distributed
};  // namespace operators
};  // namespace paddle
