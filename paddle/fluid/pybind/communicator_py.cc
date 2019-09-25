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

#include "paddle/fluid/pybind/communicator_py.h"

#include <Python.h>
#include <memory>

#include "paddle/fluid/framework/program_desc.h"
#include "pybind11/pybind11.h"

#include "paddle/fluid/operators/distributed/communicator.h"

namespace py = pybind11;

using paddle::framework::ProgramDesc;
using paddle::operators::distributed::Communicator;
using paddle::operators::distributed::AsyncCommunicator;
using paddle::operators::distributed::GeoSgdCommunicator;
using paddle::framework::Scope;

namespace paddle {
namespace pybind {

void BindCommunicator(py::module* m) {
  // Communicator is already used by nccl, change to DistCommunicator
  py::class_<Communicator, std::shared_ptr<Communicator>>(*m,
                                                          "DistCommunicator")
      .def(py::init([](const ProgramDesc& program, Scope* param_scope) {
        VLOG(0) << "using communicator";
        Communicator::InitInstance<AsyncCommunicator>(program, param_scope);
        return Communicator::GetInstantcePtr();
      }))
      .def(py::init([](const ProgramDesc& program, Scope* param_scope,
                      std::map<std::string,std::map<std::string,std::vector<std::string>>> vars_info,
                      int &trainers,int &geo_need_push_nums){
        VLOG(0) << "using geo sgd communicator";
        Communicator::InitInstance<GeoSgdCommunicator>(program, param_scope,vars_info,trainers,geo_need_push_nums);
        return Communicator::GetInstantcePtr();
      }))
      .def("stop", &Communicator::Stop)
      .def("start", &Communicator::Start)
      .def("is_running", &Communicator::IsRunning);
  /*
  py::class_<Communicator, std::shared_ptr<Communicator>>(*m,
                                                          "GeoSgdCommunicator")
      .def(py::init([](const ProgramDesc& program, Scope* param_scope,
                      std::map<std::string,std::map<std::string,std::vector<std::string>>> vars_info){
        VLOG(0) << "using geo sgd communicator";
        Communicator::GeoSgdInit(program, param_scope,vars_info);
        return Communicator::GetInstantcePtr();
      }))
      .def("stop", &Communicator::Stop)
      .def("start", &Communicator::Start)
      .def("is_running", &Communicator::IsRunning);
  */
}

}  // namespace pybind
}  // namespace paddle
