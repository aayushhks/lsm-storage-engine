#include "compaction.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_util.h"

namespace lsm {
namespace {

// Index of the live iterator positioned at the smallest key, or -1 if all are
// exhausted.
int SmallestKeyIter(const std::vector<SSTableIterator>& iters) {
  int min_iter = -1;
  for (std::size_t i = 0; i < iters.size(); ++i) {
    if (!iters[i].Valid()) {
      continue;
    }
    if (min_iter < 0 || iters[i].key() < iters[static_cast<std::size_t>(min_iter)].key()) {
      min_iter = static_cast<int>(i);
    }
  }
  return min_iter;
}

// Emits the winning version of `key` (the newest input holding it) into the
// builder, then advances every iterator past `key`.
Status EmitMergedKey(std::vector<SSTableIterator>& iters, std::string_view key,
                     SSTableBuilder* builder, bool drop_tombstones) {
  ValueTag tag = ValueTag::kValue;
  std::string value;
  bool found = false;
  for (SSTableIterator& it : iters) {
    if (!it.Valid() || it.key() != key) {
      continue;
    }
    if (!found) {
      tag = it.tag();
      value.assign(it.value());  // copy before Next() reloads the block buffer
      found = true;
    }
    it.Next();
    if (!it.status().ok()) {
      return it.status();
    }
  }
  if (!drop_tombstones || tag != ValueTag::kTombstone) {
    builder->Add(key, tag, value);
  }
  return Status::Ok();
}

}  // namespace

Status CompactTables(const std::vector<std::shared_ptr<SSTableReader>>& inputs_newest_first,
                     const std::string& out_path, const Options& options, bool drop_tombstones) {
  std::vector<SSTableIterator> iters;
  iters.reserve(inputs_newest_first.size());
  for (const auto& reader : inputs_newest_first) {
    iters.emplace_back(reader);
    iters.back().SeekToFirst();
    if (!iters.back().status().ok()) {
      return iters.back().status();
    }
  }

  constexpr std::size_t kBlockSize = 4096;
  SSTableBuilder builder(kBlockSize, options.bloom_fpr, options.enable_bloom_filters);

  while (true) {
    const int min_iter = SmallestKeyIter(iters);
    if (min_iter < 0) {
      break;  // every input exhausted
    }
    // Own the key: advancing iterators invalidates their block buffers.
    const std::string key(iters[static_cast<std::size_t>(min_iter)].key());
    const Status s = EmitMergedKey(iters, key, &builder, drop_tombstones);
    if (!s.ok()) {
      return s;
    }
  }

  return WriteFileSync(out_path, builder.Finish());
}

}  // namespace lsm
