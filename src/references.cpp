#include "detail.hpp"
#include <algorithm>
#include <random>
namespace magritte {
namespace {
std::string freshId() {
    static const char hex[] = "0123456789abcdef";
    std::random_device random;
    std::string id = "auto.description.";
    for (int i = 0; i < 32; ++i) id += hex[random() & 15];
    return id;
}
}
namespace detail {
std::shared_ptr<Description> storedReference(const Description &owner) {
    auto target = owner.reference.lock();
    std::weak_ptr<Description> empty;
    if (!target && (owner.reference.owner_before(empty) || empty.owner_before(owner.reference)))
        throw Error("Reference description expired; retain explicit references in the graph");
    return target;
}
std::shared_ptr<Description> referenceTarget(const Description &owner) {
    auto target = storedReference(owner);
    if (!target) throw Error("Reference description is uninitialized");
    return target;
}
void copyReferenceStorage(Description &copy, const Description &source, std::shared_ptr<Description> target) {
    copy.setReference(target);
    if (target && source.defaultReference_ == storedReference(source)) copy.defaultReference_ = target;
}
} // namespace detail
Description &Description::setReference(std::shared_ptr<Description> target) {
    if (!isReference()) throw Error("Reference setter requires a reference description");
    reference = target;
    if (defaultReference_ != target) defaultReference_.reset();
    return *this;
}
std::shared_ptr<Description> Description::referenceDescription() const {
    if (detail::descriptionIs(*this, "MABooleanDescription")) {
        auto owner = weak_from_this().lock();
        if (owner) return std::const_pointer_cast<Description>(owner);
        // A stack/borrowed Description has no shared owner. Its handle borrows the receiver.
        return std::shared_ptr<Description>(const_cast<Description *>(this), [](Description *) {});
    }
    if (!isReference()) throw Error("Reference getter requires a reference or Boolean description");
    auto target = detail::storedReference(*this);
    if (!target) {
        target = std::make_shared<Description>();
        target->id = freshId();
        defaultReference_ = target;
        reference = target;
    } else if (defaultReference_ != target) defaultReference_.reset();
    auto block = displayBlock;
    auto setting = properties.find("displayBlockOrSymbol");
    // Default inheritance clears an override without adding a new wire feature to legacy graphs.
    if (block)
        target->properties["displayBlockOrSymbol"] = Value();
    else if (setting != properties.end())
        target->properties["displayBlockOrSymbol"] = setting->second;
    else
        target->properties.erase("displayBlockOrSymbol");
    target->displayBlock = std::move(block);
    return target;
}
Description &Description::setUndefined(Value value, Limits limits) {
    if (!value.isNil() && !std::holds_alternative<std::string>(value.data))
        throw Error("Undefined text must be String or nil");
    detail::checkLocalValue(value, limits);
    std::size_t work = 0;
    std::function<void(Description &, std::size_t)> apply = [&](Description &current,
                                                                std::size_t depth) {
        if (++work > limits.visits || depth > limits.depth)
            throw Error("Undefined propagation work/depth limit");
        current.properties["undefined"] = value;
        if (current.isOption()) {
            // Upstream sends reference once for the nil test, then again for the setter.
            (void)current.referenceDescription();
            auto target = current.referenceDescription();
            apply(*target, depth + 1);
        }
    };
    apply(*this, 0);
    return *this;
}
} // namespace magritte

namespace magritte {
std::vector<std::string> Description::allClasses(Limits limits) const {
    if (!isRelation()) throw Error("Class choices require a relation description");
    if (classes.size() > limits.items || classes.size() > limits.visits) throw Error("Relation class limit");
    auto &registry = ClassRegistry::global();
    std::map<std::string, std::string> labels;
    for (const auto &name : classes) {
        auto info = registry.find(name);
        if (!info) throw Error("Unknown relation class: " + name);
        labels[name] = info->label;
    }
    auto result = classes;
    std::size_t work = classes.size();
    auto key = [&](const std::string &name) {
        auto text = labels.at(name);
        if (text.size() > limits.bytes || text.size() > limits.visits - work) throw Error("Relation label limit");
        work += text.size();
        for (auto &c : text) {
            if (static_cast<unsigned char>(c) >= 128) throw Error("Non-ASCII relation class sorting is not implemented");
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        }
        return text;
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
        if (++work > limits.visits) throw Error("Relation sorting work limit");
        auto left = key(a), right = key(b);
        return left < right;
    });
    return result;
}
std::shared_ptr<Description> Description::descriptionContainer() const {
    auto info = ClassRegistry::global().find(className);
    if (!info) throw Error("Unknown description class");
    auto container = std::make_shared<Description>();
    container->id = freshId();
    container->className = "MAPriorityContainer";
    container->properties["label"] = Value(info->label);
    return container;
}
RelationCommonClass Description::commonClass(Limits limits) const {
    if (!isRelation()) throw Error("Common class requires a relation description");
    if (classes.size() > limits.items || classes.size() > limits.visits) throw Error("Relation class limit");
    if (classes.empty()) return descriptionContainer();
    auto common = ClassRegistry::global().commonSuperclass(classes);
    if (!common) throw Error("Relation classes have no common class");
    return Value::Class{*common};
}
} // namespace magritte
