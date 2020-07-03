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

#include <iostream>
#include <string>
#include <vector>

namespace paddle {
namespace operators {
namespace distributed {

struct CommContext {
  CommContext() = default;

  CommContext(const std::string &name, const std::vector<std::string> &names,
              const std::vector<std::string> &emap,
              const std::vector<int64_t> &sections,
              const std::vector<std::string> &origin_names, int id,
              bool merge_add_ = true, bool queue_ = true)
      : var_name(name),
        splited_varnames(names),
        epmap(emap),
        height_sections(sections),
        origin_varnames(origin_names),
        trainer_id(id),
        merge_add(merge_add_),
        queue(queue_) {}

  CommContext(const CommContext &ctx) {
    var_name = ctx.var_name;
    splited_varnames = ctx.splited_varnames;
    epmap = ctx.epmap;
    height_sections = ctx.height_sections;
    trainer_id = ctx.trainer_id;
    merge_add = ctx.merge_add;
    queue = ctx.queue;
    origin_varnames = ctx.origin_varnames;
  }

  std::string print() const {
    std::stringstream ss;

    ss << "\nmerged_var: " << var_name << " trainer_id: " << trainer_id << "\n";

    for (size_t i = 0; i < splited_varnames.size(); i++) {
      ss << "slice varname: " << splited_varnames[i] << " ep: " << epmap[i]
         << " section: " << height_sections[i] << "\n";
    }

    ss << "origin varnames: ";
    for (size_t i = 0; i < origin_varnames.size(); i++) {
      ss << origin_varnames[i] << " ";
    }

    return ss.str();
  }

  std::string var_name;
  std::vector<std::string> splited_varnames;
  std::vector<std::string> epmap;
  std::vector<int64_t> height_sections;
  std::vector<std::string> origin_varnames;
  int trainer_id;
  bool merge_add;
  bool queue;
};

}  // namespace distributed
}  // namespace operators
}  // namespace paddle
