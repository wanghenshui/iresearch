////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "analysis/token_attributes.hpp"
#include "search/cost.hpp"
#include "search/score.hpp"
#include "utils/attribute_helper.hpp"
#include "utils/type_limits.hpp"

namespace irs {

// Adapter to use doc_iterator with conjunction and disjunction.
template<typename DocIterator>
struct score_iterator_adapter {
  using doc_iterator_t = DocIterator;

  score_iterator_adapter() noexcept = default;
  score_iterator_adapter(doc_iterator_t&& it) noexcept
    : it{std::move(it)},
      doc{irs::get<irs::document>(*this->it)},
      score{&irs::score::get(*this->it)} {
    IRS_ASSERT(doc);
  }

  score_iterator_adapter(score_iterator_adapter&&) noexcept = default;
  score_iterator_adapter& operator=(score_iterator_adapter&&) noexcept =
    default;

  typename doc_iterator_t::element_type* operator->() const noexcept {
    return it.get();
  }

  const attribute* get(irs::type_info::type_id type) const noexcept {
    return it->get(type);
  }

  attribute* get_mutable(irs::type_info::type_id type) noexcept {
    return it->get_mutable(type);
  }

  operator doc_iterator_t&&() && noexcept { return std::move(it); }

  explicit operator bool() const noexcept { return it != nullptr; }

  // access iterator value without virtual call
  doc_id_t value() const noexcept { return doc->value; }

  doc_iterator_t it;
  const irs::document* doc{};
  const irs::score* score{};
};

// Conjunction of N iterators
// -----------------------------------------------------------------------------
// c |  [0] <-- lead (the least cost iterator)
// o |  [1]    |
// s |  [2]    | tail (other iterators)
// t |  ...    |
//   V  [n] <-- end
// -----------------------------------------------------------------------------
template<typename DocIterator, typename Merger>
class conjunction : public doc_iterator, private Merger, private score_ctx {
 public:
  using merger_type = Merger;
  using doc_iterator_t = score_iterator_adapter<DocIterator>;
  using doc_iterators_t = std::vector<doc_iterator_t>;

  static_assert(std::is_nothrow_move_constructible_v<doc_iterator_t>,
                "default move constructor expected");

  explicit conjunction(doc_iterators_t&& itrs, Merger&& merger = Merger{})
    : Merger{std::move(merger)},
      itrs_{[](doc_iterators_t&& itrs) {
        IRS_ASSERT(!itrs.empty());

        // sort subnodes in ascending order by their cost
        std::sort(std::begin(itrs), std::end(itrs),
                  [](const auto& lhs, const auto& rhs) noexcept {
                    return cost::extract(lhs, cost::kMax) <
                           cost::extract(rhs, cost::kMax);
                  });
        // NRVO doesn't work for function parameters
        return std::move(itrs);
      }(std::move(itrs))},
      front_{itrs_.front().it.get()},
      front_doc_{irs::get_mutable<document>(front_)} {
    IRS_ASSERT(!itrs_.empty());
    IRS_ASSERT(front_);
    IRS_ASSERT(front_doc_);
    std::get<attribute_ptr<document>>(attrs_) =
      const_cast<document*>(front_doc_);
    std::get<attribute_ptr<cost>>(attrs_) = irs::get_mutable<cost>(front_);

    if constexpr (HasScore_v<Merger>) {
      prepare_score();
    }
  }

  auto begin() const noexcept { return std::begin(itrs_); }
  auto end() const noexcept { return std::end(itrs_); }

  // size of conjunction
  size_t size() const noexcept { return itrs_.size(); }

  attribute* get_mutable(irs::type_info::type_id type) noexcept final {
    return irs::get_mutable(attrs_, type);
  }

  doc_id_t value() const final { return front_doc_->value; }

  bool next() override {
    if (!front_->next()) {
      return false;
    }

    return !doc_limits::eof(converge(front_doc_->value));
  }

  doc_id_t seek(doc_id_t target) override {
    if (doc_limits::eof(target = front_->seek(target))) {
      return doc_limits::eof();
    }

    return converge(target);
  }

 private:
  using attributes =
    std::tuple<attribute_ptr<document>, attribute_ptr<cost>, score>;

  void prepare_score() {
    IRS_ASSERT(Merger::size());

    auto& score = std::get<irs::score>(attrs_);

    // copy scores into separate container
    // to avoid extra checks
    scores_.reserve(itrs_.size());
    for (auto& it : itrs_) {
      // FIXME(gnus): remove const cast
      auto* sub_score = const_cast<irs::score*>(it.score);
      IRS_ASSERT(score);  // ensured by score_iterator_adapter
      if (!sub_score->IsDefault()) {
        scores_.emplace_back(sub_score);
      }
    }

    // prepare score
    switch (scores_.size()) {
      case 0:
        IRS_ASSERT(score.IsDefault());
        score = ScoreFunction::Default(Merger::size());
        break;
      case 1:
        score = std::move(*scores_.front());
        break;
      case 2:
        score.Reset(*this, [](score_ctx* ctx, score_t* res) noexcept {
          auto& self = *static_cast<conjunction*>(ctx);
          auto& merger = static_cast<Merger&>(self);
          (*self.scores_.front())(res);
          (*self.scores_.back())(merger.temp());
          merger(res, merger.temp());
        });
        break;
      case 3:
        score.Reset(*this, [](score_ctx* ctx, score_t* res) noexcept {
          auto& self = *static_cast<conjunction*>(ctx);
          auto& merger = static_cast<Merger&>(self);
          (*self.scores_.front())(res);
          (*self.scores_[1])(merger.temp());
          merger(res, merger.temp());
          (*self.scores_.back())(merger.temp());
          merger(res, merger.temp());
        });
        break;
      default:
        score.Reset(*this, [](score_ctx* ctx, score_t* res) noexcept {
          auto& self = *static_cast<conjunction*>(ctx);
          auto& merger = static_cast<Merger&>(self);
          auto begin = std::begin(self.scores_);
          auto end = std::end(self.scores_);

          (**begin)(res);
          for (++begin; begin != end; ++begin) {
            (**begin)(merger.temp());
            merger(res, merger.temp());
          }
        });
        break;
    }
  }

  // tries to converge front_ and other iterators to the specified target.
  // if it impossible tries to find first convergence place
  doc_id_t converge(doc_id_t target) {
    IRS_ASSERT(!doc_limits::eof(target));

    for (auto rest = seek_rest(target); target != rest;
         rest = seek_rest(target)) {
      target = front_->seek(rest);
      if (doc_limits::eof(target)) {
        break;
      }
    }

    return target;
  }

  // seeks all iterators except the
  // first to the specified target
  doc_id_t seek_rest(doc_id_t target) {
    IRS_ASSERT(!doc_limits::eof(target));

    for (auto it = itrs_.begin() + 1, end = itrs_.end(); it != end; ++it) {
      const auto doc = (*it)->seek(target);

      if (target < doc) {
        return doc;
      }
    }

    return target;
  }

  attributes attrs_;
  doc_iterators_t itrs_;
  std::vector<score*> scores_;  // valid sub-scores
  irs::doc_iterator* front_;
  const irs::document* front_doc_{};
};

// Returns conjunction iterator created from the specified sub iterators
template<typename Conjunction, typename Merger, typename... Args>
doc_iterator::ptr MakeConjunction(typename Conjunction::doc_iterators_t&& itrs,
                                  Merger&& merger, Args&&... args) {
  if (const auto size = itrs.size(); 0 == size) {
    // empty or unreachable search criteria
    return doc_iterator::empty();
  } else if (1 == size) {
    // single sub-query
    return std::move(itrs.front());
  }

  // conjunction
  return memory::make_managed<Conjunction>(
    std::move(itrs), std::forward<Merger>(merger), std::forward<Args>(args)...);
}

}  // namespace irs
