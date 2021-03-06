#include "torch/csrc/autograd/engine.h"

#include <set>
#include <THPP/THPP.h>

using thpp::Tensor;

namespace torch { namespace autograd {

auto Engine::compute_dependencies(function_queue& queue, ready_queue_type& ready) -> dependencies_type {
  dependencies_type dependencies;
  std::set<Function*> seen;
  while (queue.size() > 0) {
    auto fn = std::move(queue.back()); queue.pop_back();
    for (auto& prev_fn_pair : fn->previous_functions) {
      auto& prev_fn = prev_fn_pair.first;
      if (!prev_fn)
        continue;
      if (dynamic_cast<Variable*>(prev_fn.get()))
        continue;
      // check for stochastic function
      if (prev_fn->is_stochastic && seen.count(prev_fn.get()) == 0 && prev_fn->requires_grad) {
        ready.emplace_back(prev_fn, GradBuffer(0));
      } else if (fn->requires_grad && prev_fn->requires_grad) {
        dependencies[prev_fn.get()] += 1;
      }
      if (seen.count(prev_fn.get()) == 0) {
        seen.insert(prev_fn.get());
        queue.push_back(prev_fn);
      }
    }
  }
  return dependencies;
}

auto Engine::backward(const variable_list& variables,
                      tensor_list& grad_variables,
                      bool retain_variables) -> void {
  function_queue creators;
  ready_queue_type ready;

  bool did_leaf_backward = false;
  int size = variables.size();
  for (int i = 0; i < size; ++i) {
    auto& var = variables[i];
    auto& grad = grad_variables[i];
    if (!var->creator) {
      // If someone calls .backward() on a leaf, it's simple...
      if (var->requires_grad) {
        var->backward(std::make_shared<Variable>(std::move(grad), false, true));
        did_leaf_backward = true;
      }
    } else {
      creators.push_back(var->creator);
      if (var->creator->requires_grad) {
        GradBuffer buf(var->creator->num_outputs);
        buf.addGrad(var->output_nr, Variable::of(std::move(grad)));
        ready.emplace_front(var->creator, std::move(buf));
      }
    }
  }

  auto dependencies = compute_dependencies(creators, ready);

  if (!did_leaf_backward && ready.size() == 0) {
    throw std::runtime_error(
        "there are no graph nodes that require computing gradients");
  }

  std::unordered_map<Function*, GradBuffer> not_ready;
  while (ready.size() > 0) {
    auto ready_pair = std::move(ready.back()); ready.pop_back();
    auto& fn = ready_pair.first;

    auto grad_inputs = fn->apply(GradBuffer::variables(std::move(ready_pair.second)));
    if (!retain_variables) {
      fn->releaseVariables();
    }

    if (grad_inputs.size() != fn->previous_functions.size()) {
      std::string msg("Function returned an invalid number of gradients - expected ");
      msg += fn->previous_functions.size();
      msg += ",  but got ";
      msg += grad_inputs.size();
      throw std::runtime_error(msg);
    }

    int size = grad_inputs.size();
    for (int i = 0; i < size; ++i) {
      auto& grad_input = grad_inputs[i];
      auto& prev_fn = fn->previous_functions[i].first;
      int output_nr = fn->previous_functions[i].second;

      // null inputs have no previous_function and we skip them here
      if (!prev_fn) {
        continue;
      }

      if (auto var = dynamic_cast<Variable*>(prev_fn.get())) {
        if (var->requires_grad) {
          var->backward(grad_input);
        }
        continue;
      }

      // Stochastic functions are placed in the ready queue by
      // compute_dependencies, so we can skip them here.
      if (prev_fn->is_stochastic || !prev_fn->requires_grad) {
        continue;
      }

      // Check if the function is ready for backward
      bool is_ready = false;
      auto it = dependencies.find(prev_fn.get());
      if (it == dependencies.end()) {
        throw std::runtime_error("dependency not found");
      } else if (--it->second == 0) {
        dependencies.erase(it);
        is_ready = true;
      }

      auto not_ready_it = not_ready.find(prev_fn.get());
      if (is_ready) {
        if (not_ready_it == not_ready.end()) {
          // The function is ready and no buffers have been allocated for it
          GradBuffer prev_buffer(prev_fn->num_outputs);
          prev_buffer.addGrad(output_nr, std::move(grad_input));
          ready.emplace_front(prev_fn, std::move(prev_buffer));
        } else {
          // The function is ready and it already has a buffer allocated.
          auto prev_buffer = std::move(not_ready_it->second);
          not_ready.erase(not_ready_it);
          prev_buffer.addGrad(output_nr, std::move(grad_input));
          ready.emplace_front(prev_fn, std::move(prev_buffer));
        }
      } else {
        // Allocate a buffer if necessary and accumulate gradient
        if (not_ready_it == not_ready.end()) {
          GradBuffer prev_buffer(prev_fn->num_outputs);
          prev_buffer.addGrad(output_nr, std::move(grad_input));
          not_ready.emplace(prev_fn.get(), std::move(prev_buffer));
        } else {
          auto &prev_buffer = not_ready_it->second;
          prev_buffer.addGrad(output_nr, std::move(grad_input));
        }
      }
    }
  }

  if (!not_ready.empty()) {
    throw std::runtime_error("could not compute gradients for some functions");
  }
}

}} // namespace torch::autograd
