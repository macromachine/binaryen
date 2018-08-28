/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Simple loop invariant code motion (licm): for every none-typed
// expression in a loop, see if it conflicts with the body of the
// loop minus itself. If not, it can be moved out.
//
// Flattening is not necessary here, but may help (as separating
// out expressions may allow movng at least part of a larger whole).
//
// TODO: Loops may have "tails" - code at the end that cannot actually
//       branch back to the loop top. We should ignore invalidations
//       with that (and can ignore moving it too).
//
// TODO: This is O(N^2) now, which we can fix with an Effect analyzer
//       which can add and subtract. (Memoizing Effects in a single
//       initial pass may help further, but take a lot more memory.)
//
// TODO: Multiple passes? A single loop may in theory allow moving of
//       X after Y is moved, and we may want to mov A out of one
//       loop, then another.
//

#include <unordered_map>

#include "wasm.h"
#include "pass.h"
#include "wasm-builder.h"
#include "cfg/cfg-traversal.h"
#include "ir/effects.h"

namespace wasm {

namespace {

// Each basic block has a list of all interesting items in it,
// which means either a loop, or an item we can move out of a loop.
struct Info {
  std::vector<Expression**> items;
};

struct LoopInvariantCodeMotion : public WalkerPass<CFGWalker<LoopInvariantCodeMotion, UnifiedExpressionVisitor<LoopInvariantCodeMotion>, Info>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new LoopInvariantCodeMotion; }

  // main entry point

  void doWalkFunction(Function* func) {
    // Create the CFG by walking the IR.
    CFGWalker<LoopInvariantCodeMotion, UnifiedExpressionVisitor<LoopInvariantCodeMotion>, Info>::doWalkFunction(func);
    // Find and move the code we can move.
    findAndMove(func);
  }

  // Track which loop a node is nested in. This is necessary because
  // the CFG may show as as being in the same basic block without
  // actually being nested, if there is no branch in the loop node.
  std::unordered_map<Expression*, Loop*> expressionLoops;

  void visitExpression(Expression* curr) {
    if (!currBasicBlock) return;
    if (!curr->is<Loop>()) {
      // Check if there is a loop parent.
      auto i = controlFlowStack.size();
      if (i == 0) return;
      i--;
      while (1) {
        if (auto* loop = controlFlowStack[i]->dynCast<Loop>()) {
          currBasicBlock->contents.items.push_back(getCurrentPointer());
          expressionLoops[curr] = loop;
          break;
        }
        if (i == 0) {
          break;
        }
        i--;
      }
    }
  }

  static void doStartLoop(LoopInvariantCodeMotion* self, Expression** currp) {
    if (self->currBasicBlock) {
      self->currBasicBlock->contents.items.push_back(currp);
    }
    CFGWalker<LoopInvariantCodeMotion, UnifiedExpressionVisitor<LoopInvariantCodeMotion>, Info>::doStartLoop(self, currp);
  }

  // Maps each loop to code we have managed to move out of it.
  std::unordered_map<Loop*, std::vector<Expression*>> movedCode;

  void findAndMove(Function* func) {
    // We can only move code if it is unconditionally run at the
    // start of the loop. Once we see potential branching, we
    // must stop.
    // Each basic block is a bunch of linear code, and may have
    // a single successor which means more linear code.
    std::vector<Expression**> loops;
    for (auto& startBlock : basicBlocks) {
      auto* block = startBlock.get();
      Loop* loop = nullptr;
      while (1) {
        bool stop = false;
        // Go through the current block.
        for (auto**& currp : block->contents.items) {
          if (!currp) continue;
          auto* curr = *currp;
          if (auto* check = curr->dynCast<Loop>()) {
            loop = check;
            loops.push_back(currp);
          } else if (loop) {
            // Check for control flow. That would stop us.
            // Note that other side effects are ok - we will check them.
            if (EffectAnalyzer(getPassOptions(), curr).branches) {
              stop = true;
              break;
            }
            if (interestingToMove(curr) && move(currp, loop)) {
              // We may see a predeccesor of this block later, in theory.
              currp = nullptr;
            }
          }
        }
        // See if we can continue on.
        if (stop || block->out.size() != 1) {
          break;
        } else {
          block = block->out[0];
        }
      }
    }
    // The moved code is now in movedCode. Do a final pass to replace
    // loops with the moved code + the loop.
    if (movedCode.empty()) return;
    struct UpdateLoops : public PostWalker<UpdateLoops> {
      std::unordered_map<Loop*, std::vector<Expression*>>* movedCode;

      void visitLoop(Loop* curr) {
        auto& currMovedCode = (*movedCode)[curr];
        if (!currMovedCode.empty()) {
          // Finish the moving by emitting the code outside.
          Builder builder(*getModule());
          auto* ret = builder.makeBlock(currMovedCode);
          ret->list.push_back(curr);
          ret->finalize(curr->type);
          replaceCurrent(ret);
        }
      }
    } updater;
    updater.setModule(getModule());
    updater.movedCode = &movedCode;
    updater.walk(func->body);
  }

  bool interestingToMove(Expression* curr) {
    // TODO: perhaps ignore blocks? would avoid the switch block pattern
    //       with very heavy nesting
    return curr->type == none && !curr->is<Nop>();
  }

  bool move(Expression** currp, Loop* loop) {
    auto* curr = *currp;
    assert(interestingToMove(curr));
    // Verify proper nesting.
    if (expressionLoops[curr] != loop) return false;
    // Check if we have side effects we can't move out.
    EffectAnalyzer myEffects(getPassOptions(), curr);
    // If we have an effect that can happen more than once, then that
    // is immediately disaqualifying, like a call. A branch is also
    // invalid as it may not make sense to be moved up (TODO: check
    // nesting of blocks?). Otherwise, side effects are ok, so long
    // as they don't interfere with anything in the loop - for example,
    // a store is ok, as is an implicit trap, we don't care if those
    // happen (or try to happen, for a trap) more than once.
    // TODO: we can memoize nodes that were invalidated here, and
    //       carefully use that later - for example, heavily nested
    //       blocks with a call at the top could be done in linear
    //       time that way.
    if (myEffects.calls || myEffects.branches) return false;
    // Check the effects of curr versus the loop
    // without curr, to see if it depends on activity in
    // the loop.
    Nop nop; // a temporary nop, just to check
    *currp = &nop;
    EffectAnalyzer loopEffects(getPassOptions(), loop);
    // Ignore branching here - we handle that directly by only
    // considering code that is guaranteed to execute at the
    // loop start.
    loopEffects.branches = false;
    if (loopEffects.invalidates(myEffects)) {
      // We can't do it, undo.
      *currp = curr;
      return false;
    }
    // We can do it!
    movedCode[loop].push_back(curr);
    // Allocate a proper nop.
    *currp = Builder(*getModule()).makeNop();
    return true;
  }
};

} // namespace

Pass *createLoopInvariantCodeMotionPass() {
  return new LoopInvariantCodeMotion();
}

} // namespace wasm

