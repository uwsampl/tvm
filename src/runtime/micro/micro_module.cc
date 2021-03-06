/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
*  Copyright (c) 2019 by Contributors
* \file micro_module.cc
*/

#include <tvm/runtime/registry.h>
#include <tvm/runtime/c_runtime_api.h>
#include <tvm/runtime/module.h>
#include <unordered_map>
#include <string>
#include "micro_session.h"
#include "low_level_device.h"
#include "micro_common.h"
#include "../pack_args.h"

namespace tvm {
namespace runtime {
/*!
 * \brief module for uTVM micro devices
 */
class MicroModuleNode final : public ModuleNode {
 public:
  MicroModuleNode() {}

  ~MicroModuleNode() {}

  const char* type_key() const final {
    return "micro";
  }

  PackedFunc GetFunction(const std::string& name,
                         const std::shared_ptr<ModuleNode>& sptr_to_self) final;

  /*!
   * \brief initializes module by establishing device connection and loads binary
   * \param binary_path path of the binary to be loaded
   */
  void InitMicroModule(const std::string& binary_path) {
    session_ = MicroSession::Global();
    low_level_device_ = session_->low_level_device();
    binary_path_ = binary_path;
    binary_info_ = session_->LoadBinary(binary_path_);
    // Patch device lib pointers.
    PatchImplHole("TVMBackendAllocWorkspace");
    PatchImplHole("TVMBackendFreeWorkspace");
    PatchImplHole("TVMAPISetLastError");
  }

  /*!
   * \brief runs selected function on the micro device
   * \param func_name name of the function to be run
   * \param func_offset offset of the function to be run
   * \param args type-erased arguments passed to the function
   */
  void RunFunction(const std::string& func_name, DevBaseOffset func_offset, const TVMArgs& args) {
    if (!session_->valid()) return;

    session_->PushToExecQueue(func_offset, args);
  }

 private:
  /*! \brief module binary info */
  BinaryInfo binary_info_;
  /*! \brief path to module binary */
  std::string binary_path_;
  /*! \brief global session pointer */
  std::shared_ptr<MicroSession> session_;
  /*! \brief low-level device pointer */
  std::shared_ptr<LowLevelDevice> low_level_device_;

  SymbolMap& symbol_map() {
    return binary_info_.symbol_map;
  }

  /*!
   * \brief patches a function pointer in this module to an implementation
   * \param func_name name of the function pointer being patched
   */
  void PatchImplHole(const std::string& func_name) {
    const DevBaseOffset init_impl_offset = session_->init_symbol_map()[func_name];
    void* init_impl_addr = (low_level_device_->base_addr() + init_impl_offset).cast_to<void*>();
    std::stringstream func_name_underscore;
    func_name_underscore << func_name << "_";
    const DevBaseOffset lib_hole_offset = symbol_map()[func_name_underscore.str()];
    session_->low_level_device()->Write(lib_hole_offset, &init_impl_addr, sizeof(void*));
  }
};

class MicroWrappedFunc {
 public:
  MicroWrappedFunc(MicroModuleNode* m,
                   std::shared_ptr<MicroSession> session,
                   const std::string& func_name,
                   DevBaseOffset func_offset) {
    m_ = m;
    session_ = session;
    func_name_ = func_name;
    func_offset_ = func_offset;
  }

  void operator()(TVMArgs args, TVMRetValue* rv, void** void_args) const {
    if (!session_->valid()) return;
    m_->RunFunction(func_name_, func_offset_, args);
  }

 private:
  /*! \brief internal module */
  MicroModuleNode* m_;
  /*! \brief reference to the session for this function (to keep the session alive) */
  std::shared_ptr<MicroSession> session_;
  /*! \brief name of the function */
  std::string func_name_;
  /*! \brief offset of the function to be called */
  DevBaseOffset func_offset_;
};

PackedFunc MicroModuleNode::GetFunction(
    const std::string& name,
    const std::shared_ptr<ModuleNode>& sptr_to_self) {
  DevBaseOffset func_offset = symbol_map()[name];
  MicroWrappedFunc f(this, this->session_, name, func_offset);
  return PackFuncVoidAddr(f, std::vector<TVMType>());
}

// register loadfile function to load module from Python frontend
TVM_REGISTER_GLOBAL("module.loadfile_micro_dev")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    std::shared_ptr<MicroModuleNode> n = std::make_shared<MicroModuleNode>();
    n->InitMicroModule(args[0]);
    *rv = runtime::Module(n);
    });
}  // namespace runtime
}  // namespace tvm
