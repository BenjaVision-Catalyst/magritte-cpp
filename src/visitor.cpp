#include "detail.hpp"
#include <algorithm>
#include <set>

namespace magritte {
void Visitor::enter() {
    if (!depth_) work_ = 0;
    if (++work_ > limits_.visits || depth_ > limits_.depth)
        throw Error("Visitor work/depth limit");
    ++depth_;
}
void Visitor::visit(Description& description) {
    enter();
    try { description.acceptMagritte(*this); }
    catch (...) { leave(); throw; }
    leave();
}
void Visitor::visitAll(const std::vector<std::shared_ptr<Description>>& descriptions) {
    if (descriptions.size() > limits_.items) throw Error("Visitor item limit");
    enter();
    try {
        for (const auto& d : descriptions) {
            if (!d) throw Error("Null visited description");
            visit(*d);
        }
    } catch (...) { leave(); throw; }
    leave();
}
void Description::acceptMagritte(Visitor& visitor) {
    // Registered subclasses inherit the nearest implemented acceptMagritte: method.
    auto name = className;
    while (!name.empty()) {
        if (name == "MADescription") { visitor.visitDescription(*this); return; }
        if (name == "MAContainer") { visitor.visitContainer(*this); return; }
        if (name == "MAPriorityContainer") { visitor.visitPriorityContainer(*this); return; }
        if (name == "MAElementDescription") { visitor.visitElementDescription(*this); return; }
        if (name == "MABooleanDescription") { visitor.visitBooleanDescription(*this); return; }
        if (name == "MAClassDescription") { visitor.visitClassDescription(*this); return; }
        if (name == "MAStringDescription") { visitor.visitStringDescription(*this); return; }
        if (name == "MAMemoDescription") { visitor.visitMemoDescription(*this); return; }
        if (name == "MAPasswordDescription") { visitor.visitPasswordDescription(*this); return; }
        if (name == "MASymbolDescription") { visitor.visitSymbolDescription(*this); return; }
        if (name == "MAMagnitudeDescription") { visitor.visitMagnitudeDescription(*this); return; }
        if (name == "MANumberDescription") { visitor.visitNumberDescription(*this); return; }
        if (name == "MAReferenceDescription") { visitor.visitReferenceDescription(*this); return; }
        if (name == "MATokenDescription") { visitor.visitTokenDescription(*this); return; }
        if (name == "MAOptionDescription") { visitor.visitOptionDescription(*this); return; }
        if (name == "MASingleOptionDescription") { visitor.visitSingleOptionDescription(*this); return; }
        if (name == "MAMultipleOptionDescription") { visitor.visitMultipleOptionDescription(*this); return; }
        if (name == "MARelationDescription") { visitor.visitRelationDescription(*this); return; }
        if (name == "MAToOneRelationDescription") { visitor.visitToOneRelationDescription(*this); return; }
        if (name == "MAToManyRelationDescription") { visitor.visitToManyRelationDescription(*this); return; }
        if (name == "MAToManyScalarRelationDescription") { visitor.visitToManyScalarRelationDescription(*this); return; }
        auto info = ClassRegistry::global().find(name);
        if (!info) throw Error("Unregistered visited class: " + name);
        name = info->superclass;
    }
    throw Error("Visited class has no implemented Magritte description dispatch");
}
void Visitor::visitDescription(Description&) {}
void Visitor::visitContainer(Description& d) { visitDescription(d); }
void Visitor::visitPriorityContainer(Description& d) { visitContainer(d); }
void Visitor::visitElementDescription(Description& d) { visitDescription(d); }
void Visitor::visitBooleanDescription(Description& d) { visitElementDescription(d); }
void Visitor::visitClassDescription(Description& d) { visitElementDescription(d); }
void Visitor::visitStringDescription(Description& d) { visitElementDescription(d); }
void Visitor::visitMemoDescription(Description& d) { visitStringDescription(d); }
void Visitor::visitPasswordDescription(Description& d) { visitStringDescription(d); }
void Visitor::visitSymbolDescription(Description& d) { visitStringDescription(d); }
void Visitor::visitMagnitudeDescription(Description& d) { visitElementDescription(d); }
void Visitor::visitNumberDescription(Description& d) { visitMagnitudeDescription(d); }
void Visitor::visitReferenceDescription(Description& d) { visitElementDescription(d); }
void Visitor::visitTokenDescription(Description& d) { visitReferenceDescription(d); }
void Visitor::visitOptionDescription(Description& d) { visitReferenceDescription(d); }
void Visitor::visitSingleOptionDescription(Description& d) { visitOptionDescription(d); }
void Visitor::visitMultipleOptionDescription(Description& d) { visitOptionDescription(d); }
void Visitor::visitRelationDescription(Description& d) { visitReferenceDescription(d); }
void Visitor::visitToOneRelationDescription(Description& d) { visitRelationDescription(d); }
void Visitor::visitToManyRelationDescription(Description& d) { visitRelationDescription(d); }
void Visitor::visitToManyScalarRelationDescription(Description& d) { visitToManyRelationDescription(d); }

void DescriptionBuilder::remember(std::shared_ptr<Description> source, bool refresh) {
    std::set<const Description *> scanned;
    std::vector<std::pair<std::shared_ptr<Description>, std::size_t>> pending{{std::move(source), 0}};
    std::size_t work = 0;
    while (!pending.empty()) {
        auto entry = std::move(pending.back()); pending.pop_back();
        auto d = std::move(entry.first);
        if (!d) throw Error("Null builder description");
        if (++work > limits_.visits || entry.second > limits_.depth)
            throw Error("Builder ownership work/depth limit");
        if (!scanned.insert(d.get()).second)
            continue;
        if (owners_.count(d.get()) && !refresh)
            continue;
        auto id = identities_.find(d->id);
        if (id != identities_.end() && id->second != d.get()) throw Error("Builder identity collision");
        if ((!owners_.count(d.get()) && owners_.size() >= limits_.nodes) ||
            d->children.size() > limits_.items)
            throw Error("Builder retained node/item limit");
        if (owners_.emplace(d.get(), d).second)
            result_.retained.push_back(d);
        identities_.emplace(d->id, d.get());
        for (const auto& child : d->children) pending.emplace_back(child, entry.second + 1);
        if (d->isReference())
            if (auto ref = detail::storedReference(*d)) pending.emplace_back(ref, entry.second + 1);
    }
}
std::shared_ptr<Description> DescriptionBuilder::ownedDescription(Description& d) const {
    auto i = owners_.find(&d);
    if (i == owners_.end()) throw Error("Use buildFrom or setBuiltDescription to retain builder inputs");
    return i->second;
}
std::shared_ptr<Description> DescriptionBuilder::copyForBuild(std::shared_ptr<Description> source) {
    auto id = prefix_ + "." + std::to_string(++serial_);
    if (identities_.count(id)) throw Error("Builder generated identity collision");
    auto copy = copyDescription(std::move(source), id, limits_);
    for (const auto& d : copy.retained) remember(d);
    return copy.root;
}
void DescriptionBuilder::setBuiltDescription(std::shared_ptr<Description> d) {
    if (d)
        remember(d, true);
    result_.root = std::move(d);
}
DescriptionCopy DescriptionBuilder::buildFrom(std::shared_ptr<Description> source,
                                             const std::string& prefix, Model* targetModel) {
    if (prefix.empty() || !std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid builder identity prefix");
    remember(source);
    prefix_ = prefix;
    target = targetModel;
    targetValue.reset();
    // As upstream, buildFrom does not reset an existing builtDescription.
    visit(*source);
    return result_;
}
void DescriptionBuilder::visitContainer(Description& d) {
    DescriptionBuilder::setBuiltDescription(copyForBuild(ownedDescription(d)));
    result_.root->children.clear();
    auto children = d.orderedChildren();
    if (children.size() > limits_.items) throw Error("Builder item limit");
    for (const auto& child : children) visit(*copyForBuild(child));
}
void DescriptionBuilder::visitDescription(Description& d) {
    auto input = ownedDescription(d);
    if (!result_.root) setBuiltDescription(input);
    else {
        if (!result_.root->isContainer()) throw Error("Built scalar description cannot add another description");
        if (result_.root->children.size() >= limits_.items) throw Error("Builder item limit");
        result_.root->children.push_back(std::move(input));
    }
}
} // namespace magritte
