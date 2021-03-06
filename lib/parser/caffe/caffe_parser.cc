//===- caffe_parser.cc ----------------------------------------------------===//
//
// Copyright (C) 2019-2020 Alibaba Group Holding Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "caffe_parser.h"

#include <fcntl.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include <fstream>

#include "caffe.pb.h"
#include "halo/lib/framework/common.h"
#include "halo/lib/framework/type.h"

namespace halo {

CAFFEAttrs::CAFFEAttrs(const caffe::BlobShape& shape) {
  for (int i = 0; i < shape.dim_size(); ++i) {
    shape_.push_back(shape.dim(i));
  }
}

CAFFEParser::~CAFFEParser() {}

Status CAFFEParser::Parse(Function* function,
                          const std::vector<std::string>& file_list,
                          const armory::Opts& opts) {
  // Verify that the version of the library that we linked against is
  // compatible with the version of the headers we compiled against.
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  HLCHECK(2 == file_list.size());
  caffe::NetParameter net_param;
  Status s = ReadProtoFromTextFile(file_list.front(), &net_param);
  if (s != Status::SUCCESS) {
    return s;
  }

  caffe::NetParameter net_param_weight;
  s = ReadWeightFromCaffeModelFile(file_list.back(), &net_param_weight);
  if (s != Status::SUCCESS) {
    return s;
  }

  BasicBlockBuilder bb_builder(function);
  BasicBlock* bb = bb_builder.CreateBasicBlock("bb0");
  return Parse(bb, net_param, net_param_weight, opts);
}

Status CAFFEParser::Parse(BasicBlock* bb, const caffe::NetParameter& net_param,
                          const caffe::NetParameter& net_param_weight,
                          const armory::Opts& opts) {
  RegisterOp();
  auto function = bb->GetParent();
  ir_builder_ = std::make_unique<IRBuilder>(bb);
  arg_builder_ = std::make_unique<ArgumentBuilder>(function);
  c_builder_ = std::make_unique<ConstantBuilder>(function);
  opts_ = opts;
  return ConvertToHaloIR(net_param, net_param_weight);
}

Status CAFFEParser::ReadProtoFromTextFile(
    const std::string& file_name, google::protobuf::Message* net_param) {
  HLCHECK(net_param);

  // TODO (unknown) replace with c++ API
  int fd = open(file_name.c_str(), O_RDONLY);
  HLCHECK(fd != -1);
  google::protobuf::io::FileInputStream input(fd);
  if (!google::protobuf::TextFormat::Parse(&input, net_param)) {
    LOG(ERROR) << "Encountered error(s) when parsing caffemodel." << file_name;
    return Status::ASSERTION;
  }
  close(fd);
  return Status::SUCCESS;
}

Status CAFFEParser::ReadWeightFromCaffeModelFile(
    const std::string& file_name, google::protobuf::Message* net_param) {
  HLCHECK(net_param);

  std::ifstream ifs(file_name);
  HLCHECK(ifs.good());

  if (!net_param->ParseFromIstream(&ifs)) {
    google::protobuf::io::IstreamInputStream input_stream(&ifs);
    if (!google::protobuf::TextFormat::Parse(&input_stream, net_param)) {
      LOG(ERROR) << "Encountered error(s) when parsing " << file_name;
      return Status::ASSERTION;
    }
  }

  return Status::SUCCESS;
}

Status CAFFEParser::ConvertToHaloIR(
    const caffe::NetParameter& net_param,
    const caffe::NetParameter& net_param_weight) {
  // Process input nodes
  Status s = ConvertPlaceholderNode(net_param);
  if (s != Status::SUCCESS) {
    return s;
  }

  // Process node
  bool weight_v1 = net_param_weight.layers_size() > 0;
  if (weight_v1) {
    std::cerr << "Caffemodel uses deprecated v1 layer parameter\n";
  }
  auto layer_size_weight = weight_v1 ? net_param_weight.layers_size()
                                     : net_param_weight.layer_size();
  auto is_input_layer = [&](int i) {
    if (weight_v1) {
      return net_param_weight.layers(i).type() == caffe::V1LayerParameter::DATA;
    }
    const std::string& type = net_param_weight.layer(i).type();
    return type == "Input" || type == "ImageData" || type == "AnnotatedData" ||
           type == "Split";
  };
  // std::vector<std::unique_ptr<caffe::BlobShape>> shapes;
  for (int i = 0, j = 0, layer_size = net_param.layer_size(); i < layer_size;
       ++i, ++j) {
    VLOG(3) << "====layer====" << i << "=========";
    if (net_param.layer(i).type() == "Input") {
      ++i;
    }

    j = (j < layer_size_weight) ? j : layer_size_weight - 1;
    while (is_input_layer(j)) {
      ++j;
    }
    caffe::LayerParameter weight_layer;
    if (weight_v1) {
      auto v1layer = net_param_weight.layers(j);
      weight_layer.set_name(v1layer.name());
      for (auto& blob : v1layer.blobs()) {
        auto b = weight_layer.add_blobs();
        b->CopyFrom(blob);
        auto shape = new caffe::BlobShape();
        shape->add_dim(blob.num());
        shape->add_dim(blob.channels());
        shape->add_dim(blob.height());
        shape->add_dim(blob.width());
        b->set_allocated_shape(shape);
        // shapes.push_back(std::move(shape));
      }
    }
    s = ConvertOneNode(net_param.layer(i),
                       weight_v1 ? weight_layer : net_param_weight.layer(j));
    if (s != Status::SUCCESS) {
      return s;
    }
  }

  return Status::SUCCESS;
}

Status CAFFEParser::ConvertOneNode(
    const caffe::LayerParameter& layer_param,
    const caffe::LayerParameter& layer_param_weight) {
  Status s = Status::SUCCESS;
  auto fp = func_lists_.find(layer_param.type());
  if (fp != func_lists_.end()) {
    s = (fp->second)(layer_param, layer_param_weight);
    if (s != Status::SUCCESS) {
      return s;
    }
  } else {
    if (opts_.print_diagnostic_report) {
      CAFFEParser::WriteCSVReport(layer_param, std::cout);
      ConvertDummyNode(layer_param, layer_param_weight);
    } else {
      LOG(ERROR) << "Convert function not found, Please check if it is "
                    "supported: Name: "
                 << "[" << layer_param.name() << "], Op: ["
                 << layer_param.type() << "]";
      return Status::ASSERTION;
    }
  }
  return Status::SUCCESS;
}

void CAFFEParser::WriteCSVReport(const caffe::LayerParameter& layer_param,
                                 std::ostream& os) {
  os << "Name: [" << layer_param.name() << "], Op: [" << layer_param.type()
     << "]\n";
}

void CAFFEParser::RegisterOp(){
#include "caffe_regist_op.h.inc"
}

Status
    CAFFEParser::ConvertPlaceholderNode(const caffe::NetParameter& net_param) {
  DataType data_type = DataType::FLOAT32;
  std::vector<int64_t> shape;
  std::string cur_node_name;
  bool is_input_param_stype =
      net_param.layer(0).type() == "Input" && net_param.input_shape_size() == 0;
  if (!is_input_param_stype) {
    // TODO(unknown) may have multi inputs
    cur_node_name = net_param.input(0);
    for (int i = 0; i < net_param.input_shape_size(); ++i) {
      const caffe::BlobShape& blob_shape = net_param.input_shape(i);
      for (int j = 0; j < blob_shape.dim_size(); ++j) {
        VLOG(3) << "Input shape(" << j << "): " << blob_shape.dim(j);
        shape.emplace_back(blob_shape.dim(j));
      }
    }
  } else {
    const caffe::InputParameter& input_param = net_param.layer(0).input_param();
    cur_node_name = net_param.layer(0).name();
    for (int i = 0; i < input_param.shape_size(); ++i) {
      const caffe::BlobShape& blob_shape = input_param.shape(i);
      for (int j = 0; j < blob_shape.dim_size(); ++j) {
        VLOG(3) << "Input shape(" << j << "): " << blob_shape.dim(j);
        shape.emplace_back(blob_shape.dim(j));
      }
    }
  }
  auto arg =
      arg_builder_->CreateArgument(cur_node_name, Type(data_type, shape));
  inst_name_to_ptr_.emplace(cur_node_name, arg);

  return Status::SUCCESS;
}

std::vector<Def> CAFFEParser::GetInputOperands(
    const caffe::LayerParameter& layer_param,
    const caffe::LayerParameter& layer_param_weight) {
  std::vector<std::string> extra_inputs =
      CreateExtraOperandsOrReturn(layer_param, layer_param_weight);
  std::vector<Def> operands;
  for (size_t i = 0, operand_num = layer_param.bottom_size(); i < operand_num;
       ++i) {
    std::string input_node_name = layer_param.bottom(i);
    if (const auto it = input_to_layer_.find(input_node_name);
        it != input_to_layer_.end()) {
      input_node_name = it->second;
    }
    if (const auto it = inst_name_to_ptr_.find(input_node_name);
        it != inst_name_to_ptr_.end()) {
      operands.emplace_back(Def{it->second, 0});
    } else {
      LOG(ERROR) << layer_param.name() << " Node's" << i
                 << "th operand:" << input_node_name << " not found";
    }
  }
  size_t j = layer_param.bottom_size();
  for (const auto& name : extra_inputs) {
    const auto iter = inst_name_to_ptr_.find(name);
    if (iter != inst_name_to_ptr_.end()) {
      operands.emplace_back(Def{iter->second, 0});
    } else {
      LOG(ERROR) << layer_param.name() << " Node's" << j
                 << "th operand:" << name << " not found";
    }
    j++;
  }

  return operands;
}

void CAFFEParser::InsertIDToInstMap(const caffe::LayerParameter& layer_param,
                                    IRObject* inst) {
  const std::string layer_name = layer_param.name();
  const std::string output_name = layer_param.top(0);
  inst_name_to_ptr_.emplace(layer_name, inst);
  input_to_layer_[output_name] = layer_name;
}

const std::vector<std::string> CAFFEParser::CreateExtraOperandsOrReturn(
    const caffe::LayerParameter& layer_param,
    const caffe::LayerParameter& layer_param_weight) {
  // TODO (unknown) official caffe blob data type support float32 only,
  // support quant type
  DataType data_type = DataType::FLOAT32;
  std::vector<std::string> extra_inputs;
  for (int i = 0, blob_size = layer_param_weight.blobs().size(); i < blob_size;
       ++i) {
    const caffe::BlobProto& blob = layer_param_weight.blobs(i);
    VLOG(3) << "layer: " << layer_param_weight.name()
            << ", blob data size: " << blob.data_size();
    HLCHECK(blob.has_shape());
    std::vector<int64_t> shape;
    const caffe::BlobShape& blob_shape = blob.shape();
    for (int j = 0; j < blob_shape.dim_size(); ++j) {
      VLOG(3) << "Shape(" << j << "): " << blob_shape.dim(j);
      shape.emplace_back(blob_shape.dim(j));
    }

    int data_size = blob.data_size();
    std::vector<float> tensor_data;
    tensor_data.reserve(data_size);
    for (int j = 0; j < data_size; ++j) {
      tensor_data.emplace_back(blob.data(j));
    }

    std::string cur_node_name =
        layer_param_weight.name() + "_" + std::to_string(i);
    // std::string cur_op_name = layer_param_weight.type();
    // layer_param_weight.set_bottom(layer_param_weight.bottom_size(),
    // cur_node_name);
    extra_inputs.emplace_back(cur_node_name);
    auto inst = c_builder_->CreateConstant(cur_node_name,
                                           Type(data_type, shape), tensor_data);
    inst_name_to_ptr_.emplace(cur_node_name, inst);
  }
  return extra_inputs;
}

Status CAFFEParser::ConvertDummyNode(
    const caffe::LayerParameter& layer_param,
    const caffe::LayerParameter& layer_param_weight) {
  std::vector<Def> operands = GetInputOperands(layer_param, layer_param_weight);
  static const int max_output_nums = 1;
  auto inst = ir_builder_->CreateDummy(layer_param.name(), operands,
                                       max_output_nums, layer_param.type());
  InsertIDToInstMap(layer_param, inst);
  return Status::SUCCESS;
}

// convert to halo ir def func
#include "caffe_convert.cc.inc"

} // end namespace halo
