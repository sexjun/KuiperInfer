
#include "runtime/runtime_ir.hpp"
#include <memory>
#include <queue>
#include <deque>
#include <utility>
#include "layer/abstract/layer_factory.hpp"
#include "tick.hpp"

namespace kuiper_infer {

void RuntimeGraphShape::InitOperatorInputTensor(const std::vector<std::shared_ptr<RuntimeOperator>> &operators) {
  if (operators.empty()) {
    LOG(ERROR) << "Operators for init input shapes is empty!";
    return;
  }
  for (const auto &op : operators) {
    if (op->input_operands.empty()) {
      continue;
    } else {
      const std::map<std::string, std::shared_ptr<RuntimeOperand>> &input_operands_map = op->input_operands;
      for (const auto &input_operand_iter : input_operands_map) {
        const auto &input_operand = input_operand_iter.second;
        const auto &type = input_operand->type;
        CHECK(type == RuntimeDataType::kTypeFloat32) << "The graph only support float32 yet!";
        const auto &shapes = input_operand->shapes;
        auto &input_datas = input_operand->datas;

        const int32_t batch = shapes.at(0);
        CHECK(batch >= 0) << "Dynamic batch size is not supported!";
        CHECK(shapes.size() == 2 || shapes.size() == 4) << "Unsupported shape sizes: " << shapes.size();

        if (!input_datas.empty()) {
          CHECK(input_datas.size() == batch) << "Batch size is wrong!";
          for (int32_t i = 0; i < batch; ++i) {
            const std::vector<uint32_t> &origin_shape = input_datas.at(i)->shapes();
            const std::vector<int32_t> &current_shape = shapes;
            if (current_shape.size() == 4) {
              CHECK(origin_shape.at(0) == current_shape.at(1) &&
                  origin_shape.at(1) == current_shape.at(2) && origin_shape.at(2) == current_shape.at(3));
            } else {
              CHECK(origin_shape.at(1) == current_shape.at(1) && origin_shape.at(0) == 1 && origin_shape.at(2) == 1);
            }
          }
        } else {
          input_datas.resize(batch);
          for (int32_t i = 0; i < batch; ++i) {
            if (shapes.size() == 4) {
              input_datas.at(i) = std::make_shared<Tensor<float>>(shapes.at(1), shapes.at(2), shapes.at(3));
            } else {
              input_datas.at(i) = std::make_shared<Tensor<float>>(1, shapes.at(1), 1);
            }
          }
        }
      }
    }
  }
}

void RuntimeGraphShape::InitOperatorOutputTensor(const std::vector<pnnx::Operator *> &pnnx_operators,
                                                 const std::vector<std::shared_ptr<RuntimeOperator>> &operators) {

  CHECK(!pnnx_operators.empty() && !operators.empty());
  CHECK(pnnx_operators.size() == operators.size());
  for (uint32_t i = 0; i < pnnx_operators.size(); ++i) {
    const std::vector<pnnx::Operand *> operands = pnnx_operators.at(i)->outputs;
    CHECK(operands.size() <= 1) << "Only support one node one output yet!";
    if (operands.empty()) {
      continue;
    }
    pnnx::Operand *operand = operands.front();
    const auto &runtime_op = operators.at(i);
    CHECK(operand != nullptr) << "Operand output is null";
    const std::vector<int32_t> &shapes = operand->shape;
    const auto &output_tensors = runtime_op->output_operands;

    const int32_t batch = shapes.at(0);
    CHECK(batch >= 0) << "Dynamic batch size is not supported!";
    CHECK(shapes.size() == 2 || shapes.size() == 4) << "Unsupported shape sizes: " << shapes.size();

    if (!output_tensors) {
      std::shared_ptr<RuntimeOperand> output_operand = std::make_shared<RuntimeOperand>();
      output_operand->shapes = shapes;
      output_operand->type = RuntimeDataType::kTypeFloat32;
      output_operand->name = operand->name + "_output";
      for (int j = 0; j < batch; ++j) {
        if (shapes.size() == 4) {
          output_operand->datas.push_back(
              std::make_shared < Tensor < float >> (shapes.at(1), shapes.at(2), shapes.at(3)));
        } else {
          output_operand->datas.push_back(std::make_shared < Tensor < float >> (1, shapes.at(1), 1));
        }
      }
      runtime_op->output_operands = output_operand;
    } else {
      CHECK(batch == output_tensors->datas.size());
      //output_tensors empty
      const auto &output_tensors_datas = output_tensors->datas;
      CHECK(output_tensors->type == RuntimeDataType::kTypeFloat32);
      CHECK(output_tensors->shapes == shapes);
      for (const auto &output_tensors_data : output_tensors_datas) {
        const auto &tensor_shapes = output_tensors->shapes;
        if (shapes.size() == 4) {
          CHECK(tensor_shapes.at(1) == shapes.at(1)
                    && tensor_shapes.at(2) == shapes.at(2) && tensor_shapes.at(3) == shapes.at(3));
        } else {
          CHECK(tensor_shapes.at(0) == 1 && tensor_shapes.at(1) == shapes.at(1) && tensor_shapes.at(2) == 1);
        }
      }
    }
  }
}

RuntimeGraph::RuntimeGraph(std::string param_path, std::string bin_path)
    : param_path_(std::move(param_path)), bin_path_(std::move(bin_path)) {

}

void RuntimeGraph::set_bin_path(const std::string &bin_path) {
  this->bin_path_ = bin_path;
}

void RuntimeGraph::set_param_path(const std::string &param_path) {
  this->param_path_ = param_path;
}

const std::string &RuntimeGraph::param_path() const {
  return this->param_path_;
}

const std::string &RuntimeGraph::bin_path() const {
  return this->bin_path_;
}

bool RuntimeGraph::Init() {
  if (this->bin_path_.empty() || this->param_path_.empty()) {
    LOG(ERROR) << "The bin path or param path is empty";
    return false;
  }

  this->graph_ = std::make_unique<pnnx::Graph>();
  int load_result = this->graph_->load(param_path_, bin_path_);
  if (load_result != 0) {
    LOG(ERROR) << "Load param path and bin path error: " << param_path_ << " " << bin_path_;
    return false;
  }

  std::vector<pnnx::Operator *> operators = this->graph_->ops;
  if (operators.empty()) {
    LOG(ERROR) << "Can not read the layers' define";
    return false;
  }

  this->operators_.clear();
  for (const pnnx::Operator *op : operators) {
    if (!op) {
      LOG(ERROR) << "Meet the empty node";
      continue;
    } else {
      std::shared_ptr<RuntimeOperator> runtime_operator = std::make_shared<RuntimeOperator>();
      // 初始化算子的名称
      runtime_operator->name = op->name;
      runtime_operator->type = op->type;

      // 初始化算子中的input
      const std::vector<pnnx::Operand *> &inputs = op->inputs;
      if (!inputs.empty()) {
        InitInputOperators(inputs, runtime_operator);
      }

      // 记录输出operand中的名称
      const std::vector<pnnx::Operand *> &outputs = op->outputs;
      if (!outputs.empty()) {
        InitOutputOperators(outputs, runtime_operator);
      }

      // 初始化算子中的attribute(权重)
      const std::map<std::string, pnnx::Attribute> &attrs = op->attrs;
      if (!attrs.empty()) {
        InitGraphAttrs(attrs, runtime_operator);
      }

      // 初始化算子中的parameter
      const std::map<std::string, pnnx::Parameter> &params = op->params;
      if (!params.empty()) {
        InitGraphParams(params, runtime_operator);
      }
      this->operators_.push_back(runtime_operator);
    }
  }

  // 构建图关系
  for (const auto &current_op : this->operators_) {
    const std::vector<std::string> &output_names = current_op->output_names;
    for (const auto &next_op : this->operators_) {
      if (next_op == current_op) {
        continue;
      }
      if (std::find(output_names.begin(), output_names.end(), next_op->name) != output_names.end()) {
        current_op->output_operators.insert({next_op->name, next_op});
      }
    }
  }

  graph_state_ = GraphState::NeedBuild;
  return true;
}

void RuntimeGraph::Build(const std::string &input_name, const std::string &output_name) {
  if (graph_state_ == GraphState::NeedInit) {
    bool init_graph = Init();
    LOG_IF(FATAL, !init_graph) << "Init graph failed!";
  }

  CHECK(graph_state_ >= GraphState::NeedBuild) << "Graph status error, current state is " << int(graph_state_);
  LOG_IF(FATAL, this->operators_.empty()) << "Graph operators is empty, may be no init";

  this->input_operators_maps_.clear();
  this->output_operators_maps_.clear();

  for (const auto &kOperator : this->operators_) {
    if (kOperator->type == "pnnx.Input") {
      this->input_operators_maps_.insert({kOperator->name, kOperator});
    } else if (kOperator->type == "pnnx.Output") {
      this->output_operators_maps_.insert({kOperator->name, kOperator});
    } else {
      std::shared_ptr<Layer> layer = RuntimeGraph::CreateLayer(kOperator);
      CHECK(layer != nullptr) << "Layer create failed!";
      if (layer) {
        kOperator->layer = layer;
      }
    }
  }
  RuntimeGraphShape::InitOperatorInputTensor(this->operators_);
  RuntimeGraphShape::InitOperatorOutputTensor(graph_->ops, this->operators_);
  graph_state_ = GraphState::Complete;
  input_name_ = input_name;
  output_name_ = output_name;
}

std::vector<std::shared_ptr<Tensor<float>>> RuntimeGraph::Forward(const std::vector<std::shared_ptr<Tensor<float>>> &inputs,
                                                                  bool debug) {
  TICK(Forward);
  if (graph_state_ < GraphState::Complete) {
    LOG(FATAL) << "Graph need be build!";
  }
  CHECK(graph_state_ == GraphState::Complete) << "Graph status error, current state is " << int(graph_state_);

  std::shared_ptr<RuntimeOperator> input_op;
  if (input_operators_maps_.find(input_name_) == input_operators_maps_.end()) {
    LOG(FATAL) << "Can not find the input node: " << input_name_;
  } else {
    input_op = input_operators_maps_.at(input_name_);
  }

  std::shared_ptr<RuntimeOperator> output_op;
  if (output_operators_maps_.find(output_name_) == output_operators_maps_.end()) {
    LOG(FATAL) << "Can not find the output node: " << input_name_;
  } else {
    output_op = output_operators_maps_.at(output_name_);
  }

  std::deque<std::shared_ptr<RuntimeOperator>> operator_queue;
  operator_queue.push_back(input_op);

  while (!operator_queue.empty()) {

    std::shared_ptr<RuntimeOperator> current_op = operator_queue.front();
    operator_queue.pop_front();

    if (!current_op || current_op == output_op) {
      if (debug) {
        LOG(INFO) << "Model inference end";
      }
      break;
    }

    if (current_op == input_op) {
      const std::vector<std::shared_ptr<Tensor<float>>> &layer_output_datas = inputs;
      ProbeNextLayer(current_op, operator_queue, layer_output_datas);
    } else {
      const std::string &current_op_name = current_op->name;

      bool has_ready = CheckOperatorReady(current_op);
      if (!has_ready) {
        operator_queue.push_back(current_op);
        continue;
      }

      const std::vector<std::shared_ptr<RuntimeOperand>> &input_operand_datas = current_op->input_operands_seq;

      std::vector<std::shared_ptr<Tensor<float>>> layer_input_datas;
      for (const auto &input_operand_data : input_operand_datas) {
        for (const auto &input_data : input_operand_data->datas) {
          layer_input_datas.push_back(input_data);
        }
      }

      CHECK(!layer_input_datas.empty());

      CHECK(current_op->output_operands != nullptr);
      std::vector<std::shared_ptr<Tensor<float>>> layer_output_datas = current_op->output_operands->datas;
      const auto &start = std::chrono::steady_clock::now();

      InferStatus status = current_op->layer->Forward(layer_input_datas, layer_output_datas);
      if (debug) {
        const double duration =
            std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count();
        LOG(INFO) << current_op_name << " " << duration << "s";
      }

      CHECK(status == InferStatus::kInferSuccess)
              << current_op->layer->layer_name() << " layer forward failed, error code: " << int(status);

//      if (debug) {
//        for (const auto &output_data : layer_output_datas) {
//          LOG(INFO) << "\n" << output_data->data().slice(0);
//        }
//      }
      ProbeNextLayer(current_op, operator_queue, layer_output_datas);
    }
  }

  for (const auto &op : this->operators_) {
    op->meet_num = 0;
  }

  CHECK(output_op->input_operands.size() == 1) << "The graph only support one path to the output node yet!";
  const auto &output_op_input_operand = output_op->input_operands.begin();
  const auto &output_operand = output_op_input_operand->second;
  if (debug) {
    TOCK(Forward)
  }
  return output_operand->datas;
}

std::shared_ptr<Layer> RuntimeGraph::CreateLayer(const std::shared_ptr<RuntimeOperator> &op) {
  LOG_IF(FATAL, !op) << "Operator is empty!";
  const auto &layer = LayerRegisterer::CreateLayer(op);
  LOG_IF(FATAL, !layer) << "Layer init failed";
  return layer;
}

void RuntimeGraph::SetOpInputData(const std::vector<std::shared_ptr<Tensor<float>>> &src,
                                  const std::vector<std::shared_ptr<Tensor<float>>> &dest) {
  CHECK(src.size() == dest.size()) << "src size: " << src.size() << " dest size: " << dest.size();
  for (uint32_t i = 0; i < src.size(); ++i) {
    dest.at(i)->set_data(src.at(i)->data());
  }
}

void RuntimeGraph::InitInputOperators(const std::vector<pnnx::Operand *> &inputs,
                                      const std::shared_ptr<RuntimeOperator> &runtime_operator) {
  for (const pnnx::Operand *input : inputs) {
    if (!input) {
      continue;
    }
    const pnnx::Operator *producer = input->producer;
    std::shared_ptr<RuntimeOperand> runtime_operand = std::make_shared<RuntimeOperand>();
    runtime_operand->name = producer->name;
    runtime_operand->shapes = input->shape;

    switch (input->type) {
      case 1: {
        runtime_operand->type = RuntimeDataType::kTypeFloat32;
        break;
      }
      default: {
        LOG(FATAL) << "Unknown input operand type: " << input->type;
      }
    }
    runtime_operator->input_operands.insert({producer->name, runtime_operand});
    runtime_operator->input_operands_seq.push_back(runtime_operand);
  }
}

void RuntimeGraph::InitOutputOperators(const std::vector<pnnx::Operand *> &outputs,
                                       const std::shared_ptr<RuntimeOperator> &runtime_operator) {
  for (const pnnx::Operand *output : outputs) {
    if (!output) {
      continue;
    }
    const auto &consumers = output->consumers;
    for (const auto &c : consumers) {
      runtime_operator->output_names.push_back(c->name);
    }
  }
}

void RuntimeGraph::InitGraphParams(const std::map<std::string, pnnx::Parameter> &params,
                                   const std::shared_ptr<RuntimeOperator> &runtime_operator) {
  for (const auto &pair : params) {
    const std::string &name = pair.first;
    const pnnx::Parameter &parameter = pair.second;
    const int type = parameter.type;
    switch (type) {
      case int(RuntimeParameterType::kParameterUnknown): {
        RuntimeParameter *runtime_parameter = new RuntimeParameter;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterBool): {
        RuntimeParameterBool *runtime_parameter = new RuntimeParameterBool;
        runtime_parameter->value = parameter.b;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterInt): {
        RuntimeParameterInt *runtime_parameter = new RuntimeParameterInt;
        runtime_parameter->value = parameter.i;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterFloat): {
        RuntimeParameterFloat *runtime_parameter = new RuntimeParameterFloat;
        runtime_parameter->value = parameter.f;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterString): {
        RuntimeParameterString *runtime_parameter = new RuntimeParameterString;
        runtime_parameter->value = parameter.s;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterIntArray): {
        RuntimeParameterIntArray *runtime_parameter = new RuntimeParameterIntArray;
        runtime_parameter->value = parameter.ai;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }

      case int(RuntimeParameterType::kParameterFloatArray): {
        RuntimeParameterFloatArray *runtime_parameter = new RuntimeParameterFloatArray;
        runtime_parameter->value = parameter.af;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }
      case int(RuntimeParameterType::kParameterStringArray): {
        RuntimeParameterStringArray *runtime_parameter = new RuntimeParameterStringArray;
        runtime_parameter->value = parameter.as;
        runtime_operator->params.insert({name, runtime_parameter});
        break;
      }
      default: {
        LOG(FATAL) << "Unknown parameter type";
      }
    }
  }
}

void RuntimeGraph::InitGraphAttrs(const std::map<std::string, pnnx::Attribute> &attrs,
                                  const std::shared_ptr<RuntimeOperator> &runtime_operator) {
  for (const auto &pair : attrs) {
    const std::string &name = pair.first;
    const pnnx::Attribute &attr = pair.second;
    switch (attr.type) {
      case 1: {
        std::shared_ptr<RuntimeAttribute> runtime_attribute = std::make_shared<RuntimeAttribute>();
        runtime_attribute->type = RuntimeDataType::kTypeFloat32;
        runtime_attribute->weight_data = attr.data;
        runtime_attribute->shape = attr.shape;
        runtime_operator->attribute.insert({name, runtime_attribute});
        break;
      }
      default : {
        LOG(FATAL) << "Unknown attribute type";
      }
    }
  }
}

bool RuntimeGraph::CheckOperatorReady(const std::shared_ptr<RuntimeOperator> &op) {
  CHECK(op != nullptr);
  CHECK(op->meet_num <= op->input_operands.size());
  if (op->meet_num == op->input_operands.size()) {
    return true;
  } else {
    return false;
  }
}

void RuntimeGraph::ProbeNextLayer(const std::shared_ptr<RuntimeOperator> &current_op,
                                  std::deque<std::shared_ptr<RuntimeOperator>> &operator_queue,
                                  const std::vector<std::shared_ptr<Tensor<float>>> &layer_output_datas) {
  const auto &next_ops = current_op->output_operators;
  for (const auto &next_op : next_ops) {
    const auto &next_rt_operator = next_op.second;
    const auto &next_input_operands = next_rt_operator->input_operands;
    if (next_input_operands.find(current_op->name) != next_input_operands.end()) {
      SetOpInputData(layer_output_datas, next_input_operands.at(current_op->name)->datas);
      const auto &iter = next_input_operands.find(current_op->name);
      if (std::find(operator_queue.begin(), operator_queue.end(), next_rt_operator) == operator_queue.end()) {
        next_rt_operator->meet_num += 1;
        if (CheckOperatorReady(next_rt_operator)) {
          operator_queue.push_back(next_rt_operator);
        }
        next_rt_operator->meet_num -= 1;
      }
      next_rt_operator->meet_num += 1;
    }
  }
}
}