#include "detail.hpp"
#include <algorithm>
#include <set>
namespace magritte {
namespace {
struct Work {
    Limits limits;
    std::size_t count = 0;
    void tick(std::size_t depth = 0) {
        if (++count > limits.visits || depth > limits.depth)
            throw Error("Composition work/depth limit");
    }
    bool accessor(const Accessor &a, const Accessor &b, std::size_t depth) {
        tick(depth);
        a.check(limits);
        b.check(limits);
        if (a.className != b.className)
            return false;
        if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor" ||
            a.className == "MADelegatorAccessor") {
            if (a.steps.size() != b.steps.size())
                return false;
            for (std::size_t i = 0; i < a.steps.size(); ++i)
                if (!accessor(a.steps[i], b.steps[i], depth + 1))
                    return false;
            return true;
        }
        if (a.className == "MAPluggableAccessor")
            return a.readBlock == b.readBlock && a.writeBlock == b.writeBlock;
        if (a.className == "MANullAccessor")
            return detail::valueKey(a.uuid, limits, count) == detail::valueKey(b.uuid, limits, count);
        return detail::valueKey(Value(a.name), limits, count) ==
                   detail::valueKey(Value(b.name), limits, count) &&
               (a.className != "MASelectorAccessor" ||
                detail::valueKey(Value(a.writeName), limits, count) ==
                    detail::valueKey(Value(b.writeName), limits, count));
    }
    bool equal(const Description &a, const Description &b, std::size_t depth = 0) {
        tick(depth);
        detail::checkDescription(a);
        detail::checkDescription(b);
        if (a.nativeRead || a.nativeWrite || b.nativeRead || b.nativeWrite)
            throw Error("Local accessor callback equality is unavailable");
        if (a.className != b.className || !accessor(a.effectiveAccessor(), b.effectiveAccessor(), depth))
            return false;
        if (!a.isContainer())
            return true;
        auto x = a.orderedChildren(), y = b.orderedChildren();
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (!equal(*x[i], *y[i], depth + 1))
                return false;
        return true;
    }
};
void retain(DescriptionCopy &result, const std::shared_ptr<Description> &source, Limits l) {
    if (!source)
        throw Error("Null description");
    for (const auto &d : result.retained)
        if (d == source)
            return;
    std::map<const Description *, std::shared_ptr<Description>> owners;
    std::function<void(std::shared_ptr<Description>)> walk = [&](auto d) {
        if (!owners.emplace(d.get(), d).second)
            return;
        for (const auto &c : d->children)
            walk(c);
        if (d->isReference())
            if (auto ref = detail::storedReference(*d)) walk(ref);
    };
    std::size_t count = 0;
    visit(
        *source,
        [&](const auto &d) {
            detail::checkDescription(d);
            detail::checkPortableDescription(d, l, count);
            for (const auto &p : d.properties)
                (void)detail::valueKey(p.second, l, count);
        },
        l);
    walk(source);
    std::set<const Description *> existing;
    for (const auto &d : result.retained)
        existing.insert(d.get());
    for (const auto &p : owners)
        if (existing.insert(p.first).second)
            result.retained.push_back(p.second);
    if (result.retained.size() > l.nodes)
        throw Error("Copy retained node limit");
}
DescriptionCopy copy(std::shared_ptr<Description> source, const std::string &id, Limits l) {
    if (id.empty())
        throw Error("Copy requires a new identity");
    DescriptionCopy result;
    retain(result, source, l);
    Work work{l};
    std::set<const Description *> active;
    std::set<std::string> identities;
    for (const auto &d : result.retained)
        identities.insert(d->id);
    for (const auto &d : result.retained)
        if (d->id == id)
            throw Error("Copy identity already belongs to a retained description");
    std::function<std::shared_ptr<Description>(std::shared_ptr<Description>, const std::string &,
                                               std::size_t)>
        clone = [&](auto d, const auto &nextId, auto depth) {
            work.tick(depth);
            if (!identities.insert(nextId).second)
                throw Error("Copy generated identity collision");
            if (nextId.empty() || !std::all_of(nextId.begin(), nextId.end(),
                                               [](unsigned char c) { return c >= 33 && c <= 126; }))
                throw Error("Invalid copy identity");
            (void)detail::valueKey(Value(nextId), l, work.count);
            if (result.retained.size() >= l.nodes)
                throw Error("Copy retained node limit");
            if (!active.insert(d.get()).second)
                throw Error("Recursive reference postCopy is unsupported; copy is not a graph deep clone");
            auto n = std::make_shared<Description>(*d);
            n->id = nextId;
            if (d->isOption()) n->options = d->options.copy();
            if(d->accessorObject){n->accessorObject=AccessorObject::fromAccessor(nextId+".accessor",d->accessorObject->snapshot(l),l);n->accessor.reset();}
            result.retained.push_back(n);
            if (d->isReference()) {
                auto ref = detail::storedReference(*d);
                detail::copyReferenceStorage(*n, *d, ref ? clone(ref, nextId + ".reference", depth + 1) : nullptr);
            }
            active.erase(d.get());
            return n;
        };
    result.root = clone(source, id, 0);
    return result;
}
std::vector<std::shared_ptr<Description>> fields(const std::shared_ptr<Description> &d) {
    if (!d || !d->isContainer())
        throw Error("Composition operation requires a container");
    return d->orderedChildren();
}
} // namespace
bool Description::kindOf(const std::string &name) const {
    return ClassRegistry::global().includesBehavior(className,name);
}
bool accessorEqual(const Accessor &a, const Accessor &b, Limits limits) {
    Work work{limits};
    return work.accessor(a, b, 0);
}
bool descriptionEqual(const Description &a, const Description &b, Limits l) {
    Work work{l};
    return work.equal(a, b);
}
bool includesDescription(const Description &container, const Description &sought, Limits limits) {
    Work work{limits};
    for (const auto &child : enumerationChildren(container, limits))
        if (work.equal(*child, sought))
            return true;
    return false;
}
DescriptionCopy copyDescription(std::shared_ptr<Description> d, const std::string &id, Limits l) {
    return copy(d, id, l);
}
DescriptionCopy asContainer(std::shared_ptr<Description> d, const std::string &id, Limits l) {
    DescriptionCopy r;
    retain(r, d, l);
    if (d->isContainer())
        r.root = d;
    else {
        if (id.empty() ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
            throw Error("Invalid wrapper identity");
        for (const auto &owner : r.retained)
            if (owner->id == id)
                throw Error("Wrapper identity collision");
        if (r.retained.size() >= l.nodes)
            throw Error("Wrapper node limit");
        r.root = std::make_shared<Description>();
        r.root->className = "MAContainer";
        r.root->id = id;
        r.root->children = {d};
    }
    return r;
}
DescriptionCopy concatenate(std::shared_ptr<Description> a, std::shared_ptr<Description> b,
                            const std::string &id, Limits l) {
    auto wrap = asContainer(a, id + ".source", l);
    auto r = copy(wrap.root, id, l);
    retain(r, b, l);
    auto more = b->isContainer() ? b->orderedChildren() : std::vector<std::shared_ptr<Description>>{b};
    r.root->children.insert(r.root->children.end(), more.begin(), more.end());
    if (r.root->children.size() > l.items)
        throw Error("Composition item limit");
    return r;
}
DescriptionCopy unionDescriptions(std::shared_ptr<Description> a, std::shared_ptr<Description> b,
                                  const std::string &id, Limits l) {
    auto left = fields(a), right = fields(b);
    auto r = copy(a, id, l);
    retain(r, b, l);
    Work w{l};
    for (const auto &d : right) {
        bool contains = false;
        for (const auto &e : left)
            if (w.equal(*e, *d)) {
                contains = true;
                break;
            }
        if (!contains)
            r.root->children.push_back(d);
    }
    if (r.root->children.size() > l.items)
        throw Error("Composition item limit");
    return r;
}
DescriptionCopy intersectDescriptions(std::shared_ptr<Description> a, std::shared_ptr<Description> b,
                                      const std::string &id, Limits l) {
    auto left = fields(a), right = fields(b);
    auto r = copy(a, id, l);
    retain(r, b, l);
    r.root->children.clear();
    Work w{l};
    for (const auto &d : left) {
        bool contains = false;
        for (const auto &e : right)
            if (w.equal(*d, *e)) {
                contains = true;
                break;
            }
        if (contains) {
            bool duplicate = false;
            for (const auto &e : r.root->children)
                if (w.equal(*d, *e)) {
                    duplicate = true;
                    break;
                }
            if (!duplicate)
                r.root->children.push_back(d);
        }
    }
    return r;
}
DescriptionCopy selectDescriptions(std::shared_ptr<Description> d,
                                   const std::function<bool(const Description &)> &f, const std::string &id,
                                   Limits l) {
    auto items = fields(d);
    auto r = copy(d, id, l);
    r.root->children.clear();
    for (const auto &item : items)
        if (f(*item))
            r.root->children.push_back(item);
    return r;
}
DescriptionCopy copyWithout(std::shared_ptr<Description> d, const Description &excluded,
                            const std::string &id, Limits l) {
    Work w{l};
    return selectDescriptions(d, [&](const auto &item) { return !w.equal(item, excluded); }, id, l);
}
DescriptionCopy copyWithoutAll(std::shared_ptr<Description> d,
                               const std::vector<std::shared_ptr<Description>> &excluded,
                               const std::string &id, Limits l) {
    if (excluded.size() > l.items)
        throw Error("Excluded item limit");
    Work w{l};
    return selectDescriptions(
        d,
        [&](const auto &item) {
            for (const auto &e : excluded) {
                if (!e)
                    throw Error("Null excluded description");
                if (w.equal(item, *e))
                    return false;
            }
            return true;
        },
        id, l);
}
DescriptionCopy
collectDescriptions(std::shared_ptr<Description> d,
                    const std::function<std::shared_ptr<Description>(std::shared_ptr<Description>)> &f,
                    const std::string &id, Limits l) {
    auto items = fields(d);
    auto r = copy(d, id, l);
    r.root->children.clear();
    for (const auto &item : items) {
        auto next = f(item);
        retain(r, next, l);
        r.root->children.push_back(next);
    }
    return r;
}
DescriptionCopy copyEmpty(std::shared_ptr<Description> d, const std::string &id, Limits l) {
    auto r = copy(d, id, l);
    fields(d);
    r.root->children.clear();
    return r;
}
DescriptionCopy copyRange(std::shared_ptr<Description> d, std::size_t begin, std::size_t end,
                          const std::string &id, Limits l) {
    auto items = fields(d);
    if (begin > end || end > items.size())
        throw Error("Composition range outside children");
    auto r = copy(d, id, l);
    r.root->children.assign(items.begin() + begin, items.begin() + end);
    return r;
}
} // namespace magritte
