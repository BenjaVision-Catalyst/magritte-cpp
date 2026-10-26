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
