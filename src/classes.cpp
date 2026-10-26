#include <algorithm>
#include <magritte/magritte.hpp>
#include <mutex>
#include <set>
namespace magritte {
namespace {
using Classes = std::map<std::string, ClassInfo>;
bool same(const ClassInfo &a, const ClassInfo &b) {
    return a.name == b.name && a.superclass == b.superclass && a.isAbstract == b.isAbstract &&
           a.label == b.label;
}
void normalize(ClassInfo &info) {
    auto valid = [](const std::string &n) {
        return !n.empty() &&
               std::all_of(n.begin(), n.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
    };
    if (!valid(info.name) || (!info.superclass.empty() && !valid(info.superclass)))
        throw Error("Invalid class identity");
}
void install(Classes &classes, const std::vector<ClassInfo> &definitions) {
    for (auto info : definitions) {
        normalize(info);
        auto old = classes.find(info.name);
        if (old != classes.end() && !same(old->second, info))
            throw Error("Conflicting class registration: " + info.name);
        classes.emplace(info.name, info);
    }
    std::set<std::string> done;
    for (const auto &p : classes) {
        std::set<std::string> path;
        auto name = p.first;
        while (!name.empty() && !done.count(name)) {
            if (!path.insert(name).second)
                throw Error("Cyclic class hierarchy");
            auto i = classes.find(name);
            if (i == classes.end())
                throw Error("Unknown superclass: " + name);
            name = i->second.superclass;
        }
        done.insert(path.begin(), path.end());
    }
}
bool includes(const Classes &classes, std::string name, const std::string &ancestor) {
    if (!classes.count(ancestor))
        return false;
    while (!name.empty()) {
        auto i = classes.find(name);
        if (i == classes.end())
            return false;
        if (name == ancestor)
            return true;
        name = i->second.superclass;
    }
    return false;
}
} // namespace
struct ClassRegistry::State {
    mutable std::mutex mutex;
    Classes classes;
    std::map<std::type_index, std::string> types;
    std::map<std::string, std::type_index> names;
};
ClassRegistry::ClassRegistry() : state_(std::make_unique<State>()) {
    registerClasses(
        {{"Array", "ArrayedCollection", false, "Array"},
         {"ArrayedCollection", "SequenceableCollection", true, "Arrayed Collection"},
         {"Bag", "Collection", false, "Bag"},
         {"Behavior", "Object", false, "Behavior"},
         {"Boolean", "Object", false, "Boolean"},
         {"Character", "Magnitude", false, "Character"},
         {"Class", "ClassDescription", false, "Class"},
         {"ClassDescription", "Behavior", true, "Class Description"},
         {"Collection", "Object", true, "Collection"},
         {"Float", "Number", true, "Float"},
         {"GRObject", "Object", false, "Object"},
         {"HashedCollection", "Collection", true, "Hashed Collection"},
         {"Integer", "Number", true, "Integer"},
         {"MAAccessor", "MAObject", true, "Accessor"},
         {"MAAdaptiveModel", "MABasicObject", false, "Adaptive Model"},
         {"MABasicObject", "GRObject", false, "Basic Object"},
         {"MABooleanDescription", "MAElementDescription", false, "Boolean"},
         {"MACachedMemento", "MAMemento", false, "Cached Memento"},
         {"MACompositeAccessor", "MAChainAccessor", false, "Composite Accessor"},
         {"MACompositeConflict", "Object", false, "MACompositeConflict"},
         {"MAChainAccessor", "MADelegatorAccessor", false, "Chain Accessor"},
         {"MACheckedMemento", "MACachedMemento", false, "Checked Memento"},
         {"MAClassDescription", "MAElementDescription", false, "Class"},
         {"MACondition", "MAObject", true, "Condition"},
         {"MAConjunctiveCondition", "MACondition", false, "Conjunctive Condition"},
         {"MAContainer", "MADescription", false, "Container"},
         {"MADelegatorAccessor", "MAAccessor", false, "Delegator Accessor"},
         {"MADescription", "MAObject", true, "Description"},
         {"MADescriptionBuilder", "MAVisitor", false, "Description Builder"},
         {"MADictionaryAccessor", "MAAccessor", false, "Dictionary Accessor"},
         {"MADynamicOptions", "Object", false, "Dynamic Options"},
         {"MAElementDescription", "MADescription", true, "Element Description"},
         {"MAIdentityAccessor", "MAAccessor", false, "Identity Accessor"},
         {"MAMagnitudeDescription", "MAElementDescription", true, "Magnitude Description"},
         {"MAMemento", "MAObject", true, "Memento"},
         {"MAMemoDescription", "MAStringDescription", false, "Memo"},
         {"MAMultipleOptionDescription", "MAOptionDescription", false, "Multiple-Option"},
         {"MANegationCondition", "MACondition", false, "Negation Condition"},
         {"MANullAccessor", "MAAccessor", false, "Null Accessor"},
         {"MANumberDescription", "MAMagnitudeDescription", false, "Number"},
         {"MAObject", "MABasicObject", true, "Object"},
         {"MAOptionDescription", "MAReferenceDescription", true, "Option Description"},
         {"MAPasswordDescription", "MAStringDescription", false, "Password"},
         {"MAPluggableAccessor", "MAAccessor", false, "Pluggable Accessor"},
         {"MAPluggableCondition", "MACondition", false, "Pluggable Condition"},
         {"MAPriorityContainer", "MAContainer", false, "Priority Container"},
         {"MAPropertyAccessor", "MADictionaryAccessor", false, "Property Accessor"},
         {"MAReader", "MAStreamingVisitor", false, "Reader"},
         {"MAReferenceDescription", "MAElementDescription", true, "Reference Description"},
         {"MARelationDescription", "MAReferenceDescription", true, "Relation Description"},
         {"MASelectorAccessor", "MAAccessor", false, "Selector Accessor"},
         {"MASingleOptionDescription", "MAOptionDescription", false, "Single-Option"},
         {"MAStraightMemento", "MAMemento", false, "Straight Memento"},
         {"MAStreamingVisitor", "MAVisitor", false, "Streaming Visitor"},
         {"MAStringDescription", "MAElementDescription", false, "String"},
         {"MAStringReader", "MAReader", false, "String Reader"},
         {"MAStringWriter", "MAWriter", false, "String Writer"},
         {"MASymbolDescription", "MAStringDescription", false, "Symbol"},
         {"MAToManyRelationDescription", "MARelationDescription", false, "1:m Relation"},
         {"MAToManyScalarRelationDescription", "MAToManyRelationDescription", false,
          "1:m scalar Relation"},
         {"MAToOneRelationDescription", "MARelationDescription", false, "1:1 Relation"},
         {"MATokenDescription", "MAReferenceDescription", false, "Token"},
         {"MAValidatorVisitor", "MAVisitor", false, "Validator Visitor"},
         {"MAVariableAccessor", "MAAccessor", false, "Variable Accessor"},
         {"MAVisitor", "MABasicObject", false, "Visitor"},
         {"MAWriter", "MAStreamingVisitor", false, "Writer"},
         {"Magnitude", "Object", false, "Magnitude"},
         {"Number", "Magnitude", true, "Number"},
         {"Object", "ProtoObject", false, "Object"},
         {"OrderedCollection", "SequenceableCollection", false, "Ordered Collection"},
         {"ProtoObject", "", false, "Proto Object"},
         {"SequenceableCollection", "Collection", true, "Sequenceable Collection"},
         {"Set", "HashedCollection", false, "Set"},
         {"String", "ArrayedCollection", false, "String"},
         {"Symbol", "String", false, "Symbol"},
         {"UndefinedObject", "Object", false, "Undefined Object"}});
}
ClassRegistry::~ClassRegistry() = default;
ClassRegistry &ClassRegistry::global() {
    static ClassRegistry registry;
    return registry;
}
void ClassRegistry::registerClass(ClassInfo info) { registerClasses({std::move(info)}); }
void ClassRegistry::registerClasses(const std::vector<ClassInfo> &definitions) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto next = state_->classes;
    install(next, definitions);
    state_->classes.swap(next);
}
void ClassRegistry::registerNativeClass(std::type_index type, ClassInfo info) {
    normalize(info);
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto t = state_->types.find(type);
    auto n = state_->names.find(info.name);
    if ((t != state_->types.end() && t->second != info.name) ||
        (n != state_->names.end() && n->second != type))
        throw Error("Conflicting native class registration");
    auto next = state_->classes;
    install(next, {info});
    auto types = state_->types;
    auto names = state_->names;
    types.emplace(type, info.name);
    names.emplace(info.name, type);
    state_->classes.swap(next);
    state_->types.swap(types);
    state_->names.swap(names);
}
std::optional<std::string> ClassRegistry::classForNativeType(std::type_index type) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto i = state_->types.find(type);
    if (i == state_->types.end())
        return {};
    return i->second;
}
std::optional<ClassInfo> ClassRegistry::find(const std::string &name) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto i = state_->classes.find(name);
    if (i == state_->classes.end())
        return {};
    return i->second;
}
std::vector<std::string> ClassRegistry::allClasses() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<std::string> result;
    for (const auto &p : state_->classes)
        result.push_back(p.first);
    return result;
}
std::vector<std::string> ClassRegistry::subclasses(const std::string &name) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->classes.count(name))
        throw Error("Unknown class: " + name);
    std::vector<std::string> result;
    for (const auto &p : state_->classes)
        if (p.second.superclass == name)
            result.push_back(p.first);
    return result;
}
std::vector<std::string> ClassRegistry::allSubclasses(const std::string &name) const {
    auto result = withAllSubclasses(name);
    result.erase(std::remove(result.begin(), result.end(), name), result.end());
    return result;
}
std::vector<std::string> ClassRegistry::withAllSubclasses(const std::string &name) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->classes.count(name))
        throw Error("Unknown class: " + name);
    std::vector<std::string> result;
    for (const auto &p : state_->classes)
        if (includes(state_->classes, p.first, name))
            result.push_back(p.first);
    return result;
}
std::vector<std::string> ClassRegistry::withAllConcreteClasses(const std::string &name) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->classes.count(name))
        throw Error("Unknown class: " + name);
    std::vector<std::string> result;
    for (const auto &p : state_->classes)
        if (!p.second.isAbstract && includes(state_->classes, p.first, name))
            result.push_back(p.first);
    return result;
}
bool ClassRegistry::includesBehavior(const std::string &name, const std::string &ancestor) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return includes(state_->classes, name, ancestor);
}
bool ClassRegistry::inheritsFrom(const std::string &name, const std::string &ancestor) const {
    return name != ancestor && includesBehavior(name, ancestor);
}
std::optional<std::string> ClassRegistry::commonSuperclass(const std::vector<std::string> &names) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (names.empty())
        return {};
    for (const auto &n : names)
        if (!state_->classes.count(n))
            throw Error("Unknown class: " + n);
    auto candidate = names.front();
    while (!candidate.empty()) {
        if (std::all_of(names.begin(), names.end(),
                        [&](const auto &name) { return includes(state_->classes, name, candidate); }))
            return candidate;
        candidate = state_->classes.at(candidate).superclass;
    }
    return {};
}
} // namespace magritte
