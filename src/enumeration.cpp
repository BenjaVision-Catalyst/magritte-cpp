#include "detail.hpp"
#include <algorithm>
#include <magritte/magritte.hpp>
namespace magritte {
std::vector<std::shared_ptr<Description>> enumerationChildren(const Description &d, Limits limits) {
    if (!d.isContainer())
        throw Error("Enumeration requires a container");
    if (d.children.size() > limits.items || d.children.size() > limits.visits)
        throw Error("Container enumeration item/work limit");
    for (const auto &child : d.children)
        if (!child)
            throw Error("Null container child");
    return d.orderedChildren();
}
std::shared_ptr<Description> childAt(const Description &d, std::size_t index,
                                     const std::function<std::shared_ptr<Description>()> &absent,
                                     Limits limits) {
    auto children = enumerationChildren(d, limits);
    return index < children.size() ? children[index] : absent();
}
std::shared_ptr<Description> childAt(const Description &d, std::size_t index, Limits limits) {
    return childAt(
        d, index,
        []() -> std::shared_ptr<Description> { throw Error("Container index outside children"); },
        limits);
}
std::shared_ptr<Description>
detectDescription(const Description &d, const DescriptionPredicate &predicate,
                  const std::function<std::shared_ptr<Description>()> &absent, Limits limits) {
    for (const auto &child : enumerationChildren(d, limits))
        if (predicate(*child))
            return child;
    return absent();
}
std::shared_ptr<Description>
detectDescription(const Description &d, const DescriptionPredicate &predicate, Limits limits) {
    return detectDescription(
        d, predicate,
        []() -> std::shared_ptr<Description> { throw Error("No matching container child"); },
        limits);
}
bool allDescriptionsSatisfy(const Description &d, const DescriptionPredicate &predicate,
                            Limits limits) {
    for (const auto &child : enumerationChildren(d, limits))
        if (!predicate(*child))
            return false;
    return true;
}
bool anyDescriptionSatisfies(const Description &d, const DescriptionPredicate &predicate,
                             Limits limits) {
    for (const auto &child : enumerationChildren(d, limits))
        if (predicate(*child))
            return true;
    return false;
}
bool noDescriptionSatisfies(const Description &d, const DescriptionPredicate &predicate,
                            Limits limits) {
    return !anyDescriptionSatisfies(d, predicate, limits);
}
void forEachDescription(const Description &d,
                        const std::function<void(const std::shared_ptr<Description> &)> &body,
                        Limits limits) {
    for (const auto &child : enumerationChildren(d, limits))
        body(child);
}
void forEachDescriptionIndexed(
    const Description &d,
    const std::function<void(std::size_t, const std::shared_ptr<Description> &)> &body,
    Limits limits) {
    auto children = enumerationChildren(d, limits);
    for (std::size_t i = 0; i < children.size(); ++i)
        body(i, children[i]);
}
void forEachDescriptionSeparated(
    const Description &d, const std::function<void(const std::shared_ptr<Description> &)> &body,
    const std::function<void()> &separator, Limits limits) {
    auto children = enumerationChildren(d, limits);
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i)
            separator();
        body(children[i]);
    }
}
} // namespace magritte

namespace magritte {
namespace {
void containerWork(const Description &d, std::size_t count, Limits limits) {
    if (!d.isContainer()) throw Error("Mutation requires a container");
    if (count > limits.items || count > limits.visits) throw Error("Container mutation item/work limit");
}
void sortChildren(std::vector<std::shared_ptr<Description>> &children, Limits limits) {
    std::size_t work = children.size();
    std::stable_sort(children.begin(), children.end(), [&](const auto &a, const auto &b) {
        if (++work > limits.visits) throw Error("Container sorting work limit");
        return detail::compareNumbers(a->priorityValue(), b->priorityValue()) < 0;
    });
}
}
Description &Description::setChildren(std::vector<std::shared_ptr<Description>> next, Limits limits) {
    containerWork(*this, next.size(), limits);
    for (const auto &d : next) if (!d) throw Error("Null container child");
    if (detail::descriptionIs(*this, "MAPriorityContainer")) sortChildren(next, limits);
    children = std::move(next);
    return *this;
}
Description &Description::add(std::shared_ptr<Description> child, Limits limits) {
    containerWork(*this, children.size() + 1, limits);
    if (!child) throw Error("Null container child");
    if (!detail::descriptionIs(*this, "MAPriorityContainer")) children.push_back(std::move(child));
    else {
        // Pharo SortedCollection>>indexForInserting:, also for stale stored priorities.
        std::size_t low = 0, high = children.size(), work = children.size();
        while (low < high) {
            if (++work > limits.visits) throw Error("Container insertion work limit");
            auto mid = low + (high - low - 1) / 2;
            if (!children[mid]) throw Error("Null container child");
            if (detail::compareNumbers(children[mid]->priorityValue(), child->priorityValue()) <= 0) low = mid + 1;
            else high = mid;
        }
        children.insert(children.begin() + low, std::move(child));
    }
    return *this;
}
Description &Description::addAll(std::vector<std::shared_ptr<Description>> incoming, Limits limits) {
    if (incoming.size() > limits.items || children.size() > limits.items - incoming.size()) throw Error("Container mutation item/work limit");
    containerWork(*this, children.size() + incoming.size(), limits);
    if (detail::descriptionIs(*this, "MAPriorityContainer") && incoming.size() > children.size() / 3) {
        for (const auto &d : incoming) {
            if (!d) throw Error("Null container child");
            children.push_back(d);
        }
        // Bulk insertion has already happened if conversion/comparison fails.
        auto next = children;
        sortChildren(next, limits);
        children = std::move(next);
    } else for (const auto &d : incoming) add(d, limits);
    return *this;
}
Description &Description::remove(const Description &sought, Limits limits) {
    containerWork(*this, children.size(), limits);
    if (auto index = detail::firstEqualChild(children, sought, limits)) {
        children.erase(children.begin() + *index);
        return *this;
    }
    throw Error("Container child not found");
}
Description &Description::removeAll(Limits limits) { return setChildren({}, limits); }
Description &Description::moveUp(const Description &sought, Limits limits) {
    containerWork(*this, children.size(), limits);
    if (detail::descriptionIs(*this, "MAPriorityContainer")) throw Error("Priority containers do not implement moveUp:");
    for (std::size_t i = 0; i < children.size(); ++i) if (children[i].get() == &sought) {
        if (i) std::swap(children[i - 1], children[i]);
        break;
    }
    return *this;
}
Description &Description::moveDown(const Description &sought, Limits limits) {
    containerWork(*this, children.size(), limits);
    if (detail::descriptionIs(*this, "MAPriorityContainer")) throw Error("Priority containers do not implement moveDown:");
    for (std::size_t i = 0; i < children.size(); ++i) if (children[i].get() == &sought) {
        if (i + 1 < children.size()) std::swap(children[i], children[i + 1]);
        break;
    }
    return *this;
}
Description &Description::resort(Limits limits) {
    if (!detail::descriptionIs(*this, "MAPriorityContainer")) throw Error("Resort requires a priority container");
    return setChildren(children, limits);
}
} // namespace magritte
