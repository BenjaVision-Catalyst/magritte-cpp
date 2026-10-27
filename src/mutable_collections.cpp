#include "detail.hpp"
#include <algorithm>
#include <set>
namespace magritte {
namespace detail {
struct CollectionStorage {
  struct Node {
    std::string kind, text;
    Value::Array sequence;
    std::map<std::string, std::pair<Value, std::size_t>> hashed;
  };
  std::map<std::string, Node> nodes;
  std::size_t serial = 0;
};
} // namespace detail
namespace {
using Storage = detail::CollectionStorage;
Value edge(const Value &value, const std::shared_ptr<Storage> &owner,
           bool borrow) {
  if (value.isMutableCollection()) {
    auto ref = value.mutableCollection();
    if (ref->storage() != owner)
      throw Error("Collection links require the same native ownership store");
    return borrow ? ref->borrowed() : ref->retained();
  }
  if (auto a = std::get_if<Value::Array>(&value.data)) {
    Value::Array out;
    for (auto &e : *a)
      out.push_back(edge(e, owner, borrow));
    return Value(out);
  }
  if (auto c = std::get_if<Value::Collection>(&value.data)) {
    auto out = *c;
    for (auto &e : out.elements)
      e = edge(e, owner, borrow);
    return Value(out);
  }
  return value;
}
void immutableKey(const Value &v) {
  if (v.isMutableCollection())
    throw Error(
        "Mutable collection members are outside the stable hashed-key profile");
  if (v.data.index() == 4 || v.data.index() == 7)
    for (auto &e : v.elements())
      immutableKey(e);
}
std::string key(const Value &v, Limits l, std::size_t &work) {
  immutableKey(v);
  return detail::valueKey(v, l, work);
}
void validId(const std::string &id) {
  if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return c >= 33 && c <= 126;
      }))
    throw Error("Invalid collection identity");
}
} // namespace
CollectionStore::CollectionStore() : state_(std::make_shared<Storage>()) {}
CollectionStore::CollectionStore(std::shared_ptr<Storage> state)
    : state_(std::move(state)) {}
CollectionReference::CollectionReference(std::shared_ptr<Storage> state,
                                         std::string id, bool own)
    : owner_(own ? state : nullptr), storage_(state), id_(std::move(id)) {}
std::shared_ptr<Storage> CollectionReference::storage() const {
  auto s = storage_.lock();
  if (!s || !s->nodes.count(id_))
    throw Error("Native collection ownership expired");
  return s;
}
const void *CollectionReference::identity() const {
  auto s = storage();
  return &s->nodes.at(id_);
}
std::string CollectionReference::kind() const {
  return storage()->nodes.at(id_).kind;
}
Value CollectionReference::retained() const {
  return Value(std::shared_ptr<CollectionReference>(
      new CollectionReference(storage(), id_, true)));
}
Value CollectionReference::borrowed() const {
  return Value(std::shared_ptr<CollectionReference>(
      new CollectionReference(storage(), id_, false)));
}
Value::Value(std::shared_ptr<CollectionReference> r) : data(std::move(r)) {
  (void)mutableCollection()->identity();
}
std::shared_ptr<CollectionReference> Value::mutableCollection() const {
  auto r = std::get_if<std::shared_ptr<CollectionReference>>(&data);
  if (!r || !*r)
    throw Error("Value is not a mutable collection");
  (void)(*r)->storage();
  return *r;
}
bool Value::sameCollection(const Value &v) const {
  return isMutableCollection() && v.isMutableCollection() &&
         mutableCollection()->identity() == v.mutableCollection()->identity();
}
Value Value::shallowCopy() const { return shallowCopy(Limits{}); }
Value Value::shallowCopy(const Limits &limits) const {
  return isMutableCollection() ? mutableCollection()->shallowCopy(limits)
                               : *this;
}
bool CollectionStore::contains(const std::string &id) const {
  return state_->nodes.count(id);
}
Value CollectionStore::at(const std::string &id) const {
  if (!contains(id))
    throw Error("Unknown collection identity: " + id);
  return Value(std::shared_ptr<CollectionReference>(
      new CollectionReference(state_, id, true)));
}
bool CollectionStore::owns(const Value &v) const {
  return v.isMutableCollection() && v.mutableCollection()->storage() == state_;
}
Value CollectionStore::create(std::string kind, std::string id,
                              Value::Array values, Limits limits) {
  if (id.empty()) {
    do {
      id = "collection." + std::to_string(++state_->serial);
    } while (contains(id));
  }
  validId(id);
  if (id.size() > limits.bytes)
    throw Error("Collection identity byte limit");
  if (kind != "array" && kind != "ordered" && kind != "set" && kind != "bag" &&
      kind != "string")
    throw Error("Unsupported mutable collection kind");
  if (contains(id))
    throw Error("Duplicate collection identity");
  if (state_->nodes.size() >= limits.nodes || values.size() > limits.items)
    throw Error("Collection store limit");
  Storage::Node n;
  n.kind = kind;
  // Placeholder size makes fixed-size Array/String initialization explicit.
  if (kind == "array")
    n.sequence.resize(values.size());
  if (kind == "string")
    for (auto &v : values) {
      auto c = std::get_if<Value::Character>(&v.data);
      if (!c)
        throw Error("String requires Characters");
      detail::checkLocalValue(v, limits);
      n.text += c->utf8;
      if (n.text.size() > limits.bytes)
        throw Error("Mutable String byte limit");
    }
  state_->nodes.emplace(id, std::move(n));
  auto result = at(id);
  try {
    result.mutableCollection()->setElements(std::move(values), limits);
  } catch (...) {
    state_->nodes.erase(id);
    throw;
  }
  return result;
}
Value CollectionStore::string(std::string id, std::string text, Limits limits) {
  return create("string", std::move(id), Value(std::move(text)).elements(),
                limits);
}
Value CollectionReference::snapshot() const {
  auto s = storage();
  const auto &n = s->nodes.at(id_);
  if (n.kind == "string")
    return Value(n.text);
  Value::Array result;
  if (n.kind == "set" || n.kind == "bag")
    for (auto &p : n.hashed)
      for (std::size_t i = 0; i < p.second.second; ++i)
        result.push_back(edge(p.second.first, s, false));
  else
    for (auto &v : n.sequence)
      result.push_back(edge(v, s, false));
  return n.kind == "array" ? Value(result)
                           : Value(Value::Collection{n.kind, result});
}
void CollectionReference::setElements(Value::Array next, Limits limits) {
  if (next.size() > limits.items || next.size() > limits.visits)
    throw Error("Mutable collection item/work limit");
  auto s = storage();
  auto &n = s->nodes.at(id_);
  auto temp = n;
  std::size_t work = 0;
  for (auto &v : next)
    detail::checkRuntimeValue(v, limits, work);
  if (n.kind == "string") {
    if (next.size() != Value(n.text).collectionSize())
      throw Error("String size is fixed");
    temp.text.clear();
    for (auto &v : next) {
      auto c = std::get_if<Value::Character>(&v.data);
      if (!c || Value(c->utf8).collectionSize() != 1)
        throw Error("String requires Characters");
      temp.text += c->utf8;
      if (temp.text.size() > limits.bytes)
        throw Error("Mutable String byte limit");
    }
  } else if (n.kind == "array" || n.kind == "ordered") {
    if (n.kind == "array" && next.size() != n.sequence.size())
      throw Error("Array size is fixed");
    temp.sequence.clear();
    for (auto &v : next)
      temp.sequence.push_back(edge(v, s, true));
  } else {
    temp.hashed.clear();
    for (auto &v : next) {
      auto k = key(v, limits, work);
      auto &entry = temp.hashed[k];
      entry.first = v;
      entry.second = n.kind == "set" ? 1 : entry.second + 1;
    }
  }
  n = std::move(temp);
}
void CollectionReference::append(Value v, Limits limits) {
  if (kind() == "array" || kind() == "string")
    throw Error("Collection size is fixed");
  auto values = snapshot().elements();
  values.push_back(std::move(v));
  setElements(std::move(values), limits);
}
void CollectionReference::atPut(std::size_t index, Value v, Limits limits) {
  if (kind() == "set" || kind() == "bag")
    throw Error("Hashed collection has no indexed write");
  auto values = snapshot().elements();
  if (index >= values.size())
    throw Error("Collection index outside storage");
  values[index] = std::move(v);
  setElements(std::move(values), limits);
}
void CollectionReference::remove(const Value &v, Limits limits) {
  if (kind() == "array" || kind() == "string")
    throw Error("Collection size is fixed");
  auto values = snapshot().elements();
  auto i = std::find_if(values.begin(), values.end(),
                        [&](auto &e) { return valuesEqual(v, e, limits); });
  if (i == values.end())
    throw Error("Collection element not found");
  values.erase(i);
  setElements(std::move(values), limits);
}
void CollectionReference::clear(Limits limits) {
  if (kind() == "array" || kind() == "string")
    throw Error("Collection size is fixed");
  setElements({}, limits);
}
Value CollectionReference::shallowCopy(Limits limits) const {
  auto s = storage();
  std::string next;
  do {
    next = "copy.collection." + std::to_string(++s->serial);
  } while (s->nodes.count(next));
  auto v = snapshot();
  return CollectionStore(s).create(kind(), next, v.elements(), limits);
}
namespace detail {
void checkRuntimeValue(const Value &v, Limits limits, std::size_t &work,
                       std::size_t depth) {
  std::set<const void *> seen;
  std::function<void(const Value &, std::size_t)> walk = [&](const Value &e,
                                                             std::size_t d) {
    if (d > limits.depth)
      throw Error("Runtime value work/depth limit");
    if (e.isMutableCollection()) {
      if (++work > limits.visits)
        throw Error("Runtime value work limit");
      auto ref = e.mutableCollection();
      if (!seen.insert(ref->identity()).second)
        return;
      if (seen.size() > limits.nodes)
        throw Error("Collection node limit");
      walk(ref->snapshot(), d);
    } else if (e.data.index() == 4 || e.data.index() == 7) {
      if (++work > limits.visits)
        throw Error("Runtime value work limit");
      if (e.collectionSize() > limits.items)
        throw Error("Collection item limit");
      for (auto &child : e.elements())
        walk(child, d + 1);
    } else {
      checkLocalValue(e, limits);
      (void)valueKey(e, limits, work, d);
    }
  };
  walk(v, depth);
}
bool valuesEqual(const Value &a, const Value &b, Limits limits,
                 std::size_t &work, std::size_t depth) {
  std::set<std::pair<const void *, const void *>> active;
  std::function<bool(const Value &, const Value &, std::size_t)> equal =
      [&](const Value &x, const Value &y, std::size_t d) {
        if (++work > limits.visits || d > limits.depth)
          throw Error("Collection equality work/depth limit");
        if (x.sameCollection(y))
          return true;
        auto left =
            x.isMutableCollection() ? x.mutableCollection()->snapshot() : x;
        auto right =
            y.isMutableCollection() ? y.mutableCollection()->snapshot() : y;
        bool lseq = left.data.index() == 4 || left.data.index() == 7,
             rseq = right.data.index() == 4 || right.data.index() == 7;
        if (lseq != rseq)
          return false;
        if (lseq && rseq) {
          if (left.data.index() != right.data.index())
            return false;
          if (left.data.index() == 7 &&
              std::get<Value::Collection>(left.data).kind !=
                  std::get<Value::Collection>(right.data).kind)
            return false;
          if (left.data.index() == 7 &&
              std::get<Value::Collection>(left.data).kind != "ordered")
            return valueKey(left, limits, work, d) ==
                   valueKey(right, limits, work, d);
          auto pair = std::make_pair(
              x.isMutableCollection() ? x.mutableCollection()->identity()
                                      : nullptr,
              y.isMutableCollection() ? y.mutableCollection()->identity()
                                      : nullptr);
          bool track = pair.first || pair.second;
          if (track && !active.insert(pair).second)
            throw Error(
                "Recursive collection equality has no supported finite result");
          auto l = left.elements(), r = right.elements();
          bool result = l.size() == r.size();
          for (std::size_t i = 0; result && i < l.size(); ++i)
            result = equal(l[i], r[i], d + 1);
          if (track)
            active.erase(pair);
          return result;
        }
        return valueKey(left, limits, work, d) ==
               valueKey(right, limits, work, d);
      };
  return equal(a, b, depth);
}
} // namespace detail
bool valuesEqual(const Value &a, const Value &b, Limits limits) {
  std::size_t work = 0;
  return detail::valuesEqual(a, b, limits, work);
}
} // namespace magritte
