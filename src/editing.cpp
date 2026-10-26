#include "detail.hpp"
#include <magritte/magritte.hpp>
#include <set>

namespace magritte {
namespace {
struct Work {
    Limits limits;
    std::size_t count = 0;
    void tick() {
        if (++count > limits.visits)
            throw Error("Editing work limit");
    }
    bool equal(const Value &a, const Value &b) {
        return detail::valueKey(a, limits, count) == detail::valueKey(b, limits, count);
    }
};
bool ignorable(const Description &d, const Value &value, Work &work) {
    work.tick();
    auto undefined = d.property("undefinedValue");
    bool ignoreDefault = false;
    if (!d.flag("shouldCacheDefault") && work.equal(value, d.defaultValue()))
        ignoreDefault = work.equal(value, d.defaultValue());
    return value.isNil() || work.equal(value, undefined) || ignoreDefault;
}
Value lookup(const std::map<std::string, Value> &values, const std::string &id) {
    auto i = values.find(id);
    return i == values.end() ? Value() : i->second;
}
} // namespace
namespace detail {
bool shouldWrite(const Description &d, const Value &candidate, const Value &previous, Limits l,
                 std::size_t &count) {
    checkDescription(d);
    checkPortableDescription(d, l, count);
    if (d.isContainer())
        throw Error("Container fields do not implement MAElementDescription editing");
    Work work{l, count};
    work.tick();
    bool result = false;
    if (d.flag("visible") && !d.flag("readOnly"))
        result = !(ignorable(d, candidate, work) && ignorable(d, previous, work)) &&
                 !work.equal(candidate, previous);
    count = work.count;
    return result;
}
} // namespace detail
bool Description::isIgnorableValue(const Value &v, Limits l) const {
    detail::checkDescription(*this);
    std::size_t count = 0;
    detail::checkPortableDescription(*this, l, count);
    if (isContainer())
        throw Error("Container is not an editable element");
    Work work{l, count};
    return ignorable(*this, v, work);
}
bool Description::shouldWrite(const Value &next, const Value &previous, Limits l) const {
    std::size_t count = 0;
    return detail::shouldWrite(*this, next, previous, l, count);
}
bool Memento::kindOf(const std::string& name) const {
    return ClassRegistry::global().includesBehavior(className(),name);
}
Memento::Memento(Model &model, std::shared_ptr<Description> description, const Graph *graph, Limits limits)
    : model_(model), description_(std::move(description)), graph_(graph), limits_(limits) {
    (void)fields();
}
std::vector<std::shared_ptr<Description>> Memento::fields() const {
    if (!description_ || !description_->isContainer())
        throw Error("Memento description must be a container");
    std::size_t count = 0;
    visit(
        *description_,
        [&](const Description &d) {
            detail::checkDescription(d);
            detail::checkPortableDescription(d, limits_, count);
        },
        limits_);
    auto result = description_->orderedChildren();
    std::set<std::string> ids;
    if (result.size() > limits_.items)
        throw Error("Editing field item limit");
    for (const auto &d : result) {
        if (d->isContainer())
            throw Error(
                "Mementos edit direct element fields; use relationship references for nested objects");
        if (d->id.empty() || !ids.insert(d->id).second)
            throw Error("Editing fields require unique nonempty identities");
    }
    return result;
}
std::vector<ValidationError> Memento::validate() {
    return magritte::validate(*description_, *this, limits_, graph_);
}
Value StraightMemento::readUsing(const Description &d) const {
    auto value = model_.readUsing(d);
    if (value.isNil())
        value = d.defaultValue();
    Work work{limits_};
    (void)detail::valueKey(value, limits_, work.count);
    return value;
}
void StraightMemento::writeUsing(const Description &d, Value value) {
    if (d.shouldWrite(value, model_.readUsing(d), limits_))
        model_.writeUsing(d, std::move(value));
}
CachedMemento::CachedMemento(Model &m, std::shared_ptr<Description> d, const Graph *g, Limits l)
    : CachedMemento(m, std::move(d), g, l, true) {}
CachedMemento::CachedMemento(Model &m, std::shared_ptr<Description> d, const Graph *g, Limits l,
                             bool initialize)
    : Memento(m, std::move(d), g, l) {
    if (initialize)
        reset();
}
CachedMemento::Values CachedMemento::pullRaw() const {
    Values values;
    Work work{limits_};
    for (const auto &d : fields()) {
        work.tick();
        auto value = model_.readUsing(*d);
        (void)detail::valueKey(value, limits_, work.count);
        values.emplace(d->id, std::move(value));
    }
    return values;
}
CachedMemento::Values CachedMemento::cache() const {
    Values values;
    for (const auto &p : cache_)
        values.emplace(p.first, p.second.value);
    return values;
}
void CachedMemento::checkIdentities() const {
    for (const auto &d : fields()) {
        auto i = cache_.find(d->id);
        if (i != cache_.end() && i->second.description.get() != d.get())
            throw Error("Description identity replaced; reset the memento before reuse");
    }
}
bool CachedMemento::changed(const Values &first, const Values &second) const {
    std::size_t count = 0;
    for (const auto &d : fields())
        if (detail::shouldWrite(*d, lookup(first, d->id), lookup(second, d->id), limits_, count))
            return true;
    return false;
}
bool CachedMemento::hasChanges() const {
    checkIdentities();
    return changed(cache(), pullRaw());
}
Value CachedMemento::readUsing(const Description &d) const {
    auto i = cache_.find(d.id);
    if (i == cache_.end())
        throw Error("Description is not cached; reset or write the described field first");
    if (i->second.description.get() != &d)
        throw Error("Cached description identity mismatch");
    return i->second.value;
}
void CachedMemento::writeUsing(const Description &d, Value value) {
    checkIdentities();
    detail::checkLocalValue(value, limits_);
    Work work{limits_};
    for (const auto &p : cache_)
        if (p.first != d.id)
            (void)detail::valueKey(p.second.value, limits_, work.count);
    (void)detail::valueKey(value, limits_, work.count);
    auto i = cache_.find(d.id);
    if (i != cache_.end()) {
        if (i->second.description.get() != &d)
            throw Error("Cached description identity mismatch");
        i->second.value = std::move(value);
        return;
    }
    for (const auto &field : fields())
        if (field.get() == &d) {
            cache_.emplace(d.id, Entry{field, std::move(value)});
            order_.push_back(d.id);
            return;
        }
    throw Error("Cannot cache a foreign description; add it to the container first");
}
void CachedMemento::reset() {
    auto current = fields();
    std::map<std::string, Entry> next;
    std::vector<std::string> order;
    Work work{limits_};
    for (const auto &d : current) {
        work.tick();
        auto value = model_.readUsing(*d);
        if (work.equal(value, d->property("undefinedValue")))
            value = d->defaultValue();
        (void)detail::valueKey(value, limits_, work.count);
        next.emplace(d->id, Entry{d, std::move(value)});
        order.push_back(d->id);
    }
    cache_ = std::move(next);
    order_ = std::move(order);
}
bool CachedMemento::shouldPush(const Entry &entry, const Value &current, std::size_t &work) const {
    return detail::shouldWrite(*entry.description, entry.value, current, limits_, work);
}
void CachedMemento::commit() {
    checkIdentities();
    std::size_t work = 0;
    // Snapshot iteration order so callbacks cannot invalidate the loop's storage.
    auto pending = cache_;
    auto order = order_;
    for (const auto &id : order) {
        if (++work > limits_.visits)
            throw Error("Editing commit work limit");
        const auto &entry = pending.at(id);
        auto current = model_.readUsing(*entry.description);
        if (shouldPush(entry, current, work))
            model_.writeUsing(*entry.description, entry.value);
    }
    reset();
}
CheckedMemento::CheckedMemento(Model &m, std::shared_ptr<Description> d, const Graph *g, Limits l)
    : CachedMemento(m, std::move(d), g, l, false) {
    reset();
}
void CheckedMemento::reset() {
    CachedMemento::reset();
    original_ = pullRaw();
}
bool CheckedMemento::hasModelChangedElsewhere() const {
    checkIdentities();
    return changed(original_, pullRaw());
}
bool CheckedMemento::hasConflict() const { return hasChanges() && hasModelChangedElsewhere(); }
bool CheckedMemento::shouldPush(const Entry &entry, const Value &current, std::size_t &work) const {
    auto original = original_.find(entry.description->id);
    if (original == original_.end())
        throw Error("New field has no original value; reset checked memento first");
    auto actual = cache_.find(entry.description->id);
    if (actual == cache_.end())
        throw Error("Missing checked cache value");
    return detail::shouldWrite(*entry.description, actual->second.value, original->second, limits_, work) &&
           CachedMemento::shouldPush(entry, current, work);
}
std::vector<ValidationError> CheckedMemento::validate() {
    if (!hasConflict())
        return Memento::validate();
    reset();
    return {{description_->id, "MAConflictError", "Model changed while editing; memento reset"}};
}
} // namespace magritte
