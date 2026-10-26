#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <typeindex>
#include <type_traits>
#include <utility>

namespace magritte {
// Portable numbers are signed Int64 and finite IEEE-754 binary64.
struct Value {
    struct Class { std::string name; };
    struct Reference { std::string id; bool operator==(const Reference& r) const { return id == r.id; } };
    struct Character { std::string utf8; bool operator==(const Character& c) const { return utf8 == c.utf8; } };
    struct Symbol { std::string utf8; };
    struct Collection { std::string kind; std::vector<Value> elements; };
    using Array = std::vector<Value>;
    std::variant<std::monostate, bool, std::int64_t, std::string, Array, Reference, Character, Collection, Symbol, double, Class> data;
    Value() = default;
    explicit Value(bool v) : data(v) {}
    explicit Value(std::int64_t v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(const char* v) : data(std::string(v)) {}
    explicit Value(Array v) : data(std::move(v)) {}
    explicit Value(Reference v) : data(std::move(v)) {}
    explicit Value(Character v) : data(std::move(v)) {}
    explicit Value(Collection v) : data(std::move(v)) {}
    explicit Value(Symbol v) : data(std::move(v)) {}
    explicit Value(double v);
    explicit Value(Class v);
    bool isCollection() const;
    Array elements() const;
    std::size_t collectionSize() const;
    bool isNil() const { return std::holds_alternative<std::monostate>(data); }
    bool operator==(const Value& v) const;
    bool operator!=(const Value& v) const { return !(*this == v); }
};
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct Limits {
    std::size_t bytes = 1024 * 1024, depth = 64, nodes = 10000,
                items = 100000, visits = 100000;
};
// Runtime class identities and single-inheritance relationships, analogous to an image's class environment.
struct ClassInfo {
    std::string name, superclass;
    bool isAbstract = false;
    std::string label;
    ClassInfo() = default;
    ClassInfo(std::string name, std::string superclass = "Object", bool isAbstract = false, std::optional<std::string> label = {})
        : name(std::move(name)), superclass(std::move(superclass)), isAbstract(isAbstract), label(label.value_or(this->name)) {}
};
class ClassRegistry {
    struct State;
    std::unique_ptr<State> state_;
    ClassRegistry();
public:
    ~ClassRegistry();
    ClassRegistry(const ClassRegistry&) = delete;
    ClassRegistry& operator=(const ClassRegistry&) = delete;
    static ClassRegistry& global();
    void registerClass(ClassInfo);
    void registerClasses(const std::vector<ClassInfo>&); // Atomic, including forward superclass references.
    void registerNativeClass(std::type_index, ClassInfo);
    template<class T> void registerNativeClass(ClassInfo info) { registerNativeClass(std::type_index(typeid(T)), std::move(info)); }
    std::optional<std::string> classForNativeType(std::type_index) const;
    std::optional<ClassInfo> find(const std::string&) const;
    std::vector<std::string> allClasses() const;
    std::vector<std::string> subclasses(const std::string&) const;
    std::vector<std::string> allSubclasses(const std::string&) const;
    std::vector<std::string> withAllSubclasses(const std::string&) const;
    std::vector<std::string> withAllConcreteClasses(const std::string&) const;
    bool includesBehavior(const std::string& className, const std::string& ancestor) const;
    bool inheritsFrom(const std::string& className, const std::string& ancestor) const;
    std::optional<std::string> commonSuperclass(const std::vector<std::string>&) const;
};
struct ValidationError {
    std::string descriptionId, code, message;
};
struct AccessorObject;
struct ConditionObject;
struct Description;
struct Graph;
struct Accessor {
    std::string className = "MADictionaryAccessor", name, writeName;
    Value uuid; // MANullAccessor comparison token, preserved without interpretation
    std::vector<Accessor> steps; // MAChainAccessor, performed from left to right
    Value read(const struct Model&, const Graph* = nullptr, Limits = {}) const;
    void write(struct Model&, Value, Graph* = nullptr, Limits = {}) const;
    void check(Limits = {}) const;
};
struct Model {
    virtual ~Model() = default;
    virtual bool usesStoredContainerValues() const { return false; }
    virtual bool kindOf(const std::string& name) const { return ClassRegistry::global().includesBehavior("Object", name); }
    virtual Value readSlot(const std::string& strategy, const std::string& name) const;
    virtual void writeSlot(const std::string& strategy, const std::string& name, Value);
    virtual Value readUsing(const Description&) const = 0;
    // Raw model writes, like Smalltalk's write:using:, do not validate or enforce UI flags.
    virtual void writeUsing(const Description&, Value) = 0;
};
struct Predicate {
    std::string className = "MAPluggableCondition", selector;
    Value receiver;
    std::vector<Value> arguments;
    std::size_t index = 0;
    std::vector<Predicate> children;
    void check(Limits = {}) const;
    bool evaluate(const Value&, Limits = {}) const;
};
struct PortableCondition { std::string label; Predicate predicate; };
struct Condition {
    std::string label;
    std::function<bool(const Value&)> test;
};
class Visitor;
struct Description : Model {
    virtual void acceptMagritte(Visitor&);
    bool kindOf(const std::string&) const override;
    std::string id, className = "MAStringDescription", key;
    std::map<std::string, Value> properties;
    std::vector<std::shared_ptr<Description>> children;
    // Graph owns descriptions. Non-owning reference edges permit recursive models.
    std::weak_ptr<Description> reference;
    std::vector<std::string> classes;
    std::vector<Value> options;
    std::vector<std::pair<Value, std::string>> labels;
    std::vector<Condition> conditions;
    std::vector<PortableCondition> portableConditions;
    std::optional<Accessor> accessor;
    // Live accessor identity is local; materialize explicitly before graph export.
    std::shared_ptr<AccessorObject> accessorObject;
    void materializeAccessor(Limits = {});
    Accessor effectiveAccessor() const;
    std::function<Value(const void*)> nativeRead;
    std::function<void(void*, const Value&)> nativeWrite;
    std::function<Value()> defaultFunction;
    bool isContainer() const;
    bool isRelation() const;
    bool isOption() const;
    bool isReference() const { return isRelation() || isOption() || className == "MATokenDescription"; }
    bool isMultiple() const;
    std::shared_ptr<Description> referenceDescription() const;
    std::vector<Value> allOptions(Limits = {}) const;
    std::vector<Value> allOptionsWith(const Value&, Limits = {}) const;
    std::string labelForOption(const Value&, Limits = {}) const;
    std::string obfuscated(const Value&, Limits = {}) const;
    bool isObfuscated(const Value&) const;
    Value property(const std::string&) const;
    Value defaultValue() const;
    bool isIgnorableValue(const Value&, Limits = {}) const;
    bool shouldWrite(const Value& candidate, const Value& previous, Limits = {}) const;
    bool flag(const std::string&) const;
    std::int64_t priority() const;
    std::vector<std::shared_ptr<Description>> orderedChildren() const;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
    Value readSlot(const std::string&, const std::string&) const override;
    void writeSlot(const std::string&, const std::string&, Value) override;
};
struct AdaptiveModel : Model {
    bool usesStoredContainerValues() const override { return true; }
    std::string id;
    bool kindOf(const std::string& name) const override { return ClassRegistry::global().includesBehavior("MAAdaptiveModel", name); }
    std::shared_ptr<Description> description;
    // Keys are stable graph description identities, not field names.
    std::map<std::string, Value> values;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct DictionaryModel : Model {
    Graph* resolver = nullptr; // borrowed; required for chained reference traversal
    Value readSlot(const std::string&, const std::string&) const override;
    void writeSlot(const std::string&, const std::string&, Value) override;
    std::map<std::string, Value> values;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
class NativeModel : public Model {
    void* object_;
    std::vector<std::string> classes_;
public:
    Graph* resolver = nullptr;
    // Explicit host bindings, indexed by selector or variable name. No implicit reflection.
    std::map<std::string, std::function<Value(const void*)>> readers;
    std::map<std::string, std::function<void(void*, const Value&)>> writers;
    Value readSlot(const std::string&, const std::string&) const override;
    void writeSlot(const std::string&, const std::string&, Value) override;
    // Caller retains ownership and must outlive this adapter and every access.
    explicit NativeModel(void* object, std::vector<std::string> classes = {}) : object_(object), classes_(std::move(classes)) {}
    template<class T> static NativeModel registered(T* object) {
        if (!object) throw Error("Null registered native object");
        auto name=ClassRegistry::global().classForNativeType(std::type_index(typeid(*object)));
        if (!name) throw Error("Native type is not registered");
        void* pointer=object;
        if constexpr(std::is_polymorphic<T>::value) pointer=dynamic_cast<void*>(object);
        return NativeModel(pointer,{*name});
    }
    bool kindOf(const std::string&) const override;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct Graph {
    std::string format = "magritte.graph/3", root;
    // Local native targets are retained adapters, never portable graph data.
    std::map<std::string, std::shared_ptr<Model>> nativeObjects;
    const Model& resolve(const Value::Reference&) const;
    const Model& resolve(const Value&) const;
    Model& resolve(const Value&);
    std::map<std::string, std::shared_ptr<Description>> descriptions;
    std::map<std::string, AdaptiveModel> objects;
    static Graph decode(const std::string&, Limits = {});
    std::string encode(Limits = {}) const;
};
// Editing adapters borrow their model and resolver; both must outlive the memento.
class Memento : public Model {
protected:
    Model& model_;
    std::shared_ptr<Description> description_;
    const Graph* graph_;
    Limits limits_;
    std::vector<std::shared_ptr<Description>> fields() const;
public:
    Memento(Model&, std::shared_ptr<Description>, const Graph* = nullptr, Limits = {});
    virtual std::string className() const { return "MAMemento"; }
    bool kindOf(const std::string&) const override;
    Model& model() const { return model_; }
    const std::shared_ptr<Description>& description() const { return description_; }
    bool usesStoredContainerValues() const override { return true; }
    virtual void reset() = 0;
    virtual void commit() = 0; // Does not implicitly validate or provide a transaction.
    virtual std::vector<ValidationError> validate();
};
class StraightMemento final : public Memento {
public:
    using Memento::Memento;
    std::string className() const override { return "MAStraightMemento"; }
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
    void reset() override {}
    void commit() override {}
};
class CachedMemento : public Memento {
public:
    using Values = std::map<std::string, Value>;
protected:
    struct Entry { std::shared_ptr<Description> description; Value value; };
    std::map<std::string, Entry> cache_;
    std::vector<std::string> order_;
    void checkIdentities() const;
    CachedMemento(Model&, std::shared_ptr<Description>, const Graph*, Limits, bool initialize);
    Values pullRaw() const;
    bool changed(const Values&, const Values&) const;
    virtual bool shouldPush(const Entry&, const Value& current, std::size_t& work) const;
public:
    CachedMemento(Model&, std::shared_ptr<Description>, const Graph* = nullptr, Limits = {});
    std::string className() const override { return "MACachedMemento"; }
    Values cache() const;
    bool hasChanges() const;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
    void reset() override;
    void commit() override;
};
class CheckedMemento final : public CachedMemento {
    Values original_;
    bool shouldPush(const Entry&, const Value& current, std::size_t& work) const override;
public:
    CheckedMemento(Model&, std::shared_ptr<Description>, const Graph* = nullptr, Limits = {});
    std::string className() const override { return "MACheckedMemento"; }
    const Values& original() const { return original_; }
    bool hasModelChangedElsewhere() const;
    bool hasConflict() const;
    void reset() override;
    std::vector<ValidationError> validate() override;
};
// Expanded, class-specific metadata descriptions using upstream selector accessors.
// This returns a graph so its weak reference descriptions retain an owner.
Graph descriptionModelFor(const Description&, Limits = {});
// Magritte equality ignores IDs/properties; copies follow postCopy, not deep clone.
bool descriptionEqual(const Description&, const Description&, Limits = {});
struct DescriptionCopy {
    std::shared_ptr<Description> root;
    // Own copied reference descriptions and shared descendants/references.
    std::vector<std::shared_ptr<Description>> retained;
};
// MAVisitor dispatches one description. Child/reference traversal belongs to the visitor.
class Visitor {
    std::size_t depth_ = 0, work_ = 0;
    void enter();
    void leave() noexcept { --depth_; }
protected:
    Limits limits_;
public:
    explicit Visitor(Limits limits = {}) : limits_(limits) {}
    virtual ~Visitor() = default;
    virtual void visit(Description&);
    virtual void visitAll(const std::vector<std::shared_ptr<Description>>&);
    virtual void visitDescription(Description&);
    virtual void visitContainer(Description&);
    virtual void visitPriorityContainer(Description&);
    virtual void visitElementDescription(Description&);
    virtual void visitBooleanDescription(Description&);
    virtual void visitClassDescription(Description&);
    virtual void visitStringDescription(Description&);
    virtual void visitMemoDescription(Description&);
    virtual void visitPasswordDescription(Description&);
    virtual void visitSymbolDescription(Description&);
    virtual void visitMagnitudeDescription(Description&);
    virtual void visitNumberDescription(Description&);
    virtual void visitReferenceDescription(Description&);
    virtual void visitTokenDescription(Description&);
    virtual void visitOptionDescription(Description&);
    virtual void visitSingleOptionDescription(Description&);
    virtual void visitMultipleOptionDescription(Description&);
    virtual void visitRelationDescription(Description&);
    virtual void visitToOneRelationDescription(Description&);
    virtual void visitToManyRelationDescription(Description&);
    virtual void visitToManyScalarRelationDescription(Description&);
};
// MADescriptionBuilder's stateful copy/enrichment protocol. Model pointers are borrowed.
class DescriptionBuilder : public Visitor {
    DescriptionCopy result_;
    std::map<const Description*, std::shared_ptr<Description>> owners_;
    std::map<std::string, const Description*> identities_;
    std::string prefix_;
    std::size_t serial_ = 0;
    void remember(std::shared_ptr<Description>);
    std::shared_ptr<Description> copyForBuild(std::shared_ptr<Description>);
protected:
    std::shared_ptr<Description> ownedDescription(Description&) const;
public:
    using Visitor::Visitor;
    Model* target = nullptr;
    Model* model = nullptr;
    std::int64_t priority = 0;
    const std::shared_ptr<Description>& builtDescription() const { return result_.root; }
    void setBuiltDescription(std::shared_ptr<Description>);
    // New IDs use newIdPrefix.N. A suppressed visit can leave result.root null.
    DescriptionCopy buildFrom(std::shared_ptr<Description>, const std::string& newIdPrefix, Model* target = nullptr);
    void visitContainer(Description&) override;
    void visitDescription(Description&) override;
};
DescriptionCopy copyDescription(std::shared_ptr<Description>, const std::string& newId, Limits = {});
DescriptionCopy asContainer(std::shared_ptr<Description>, const std::string& newId, Limits = {});
DescriptionCopy concatenate(std::shared_ptr<Description>, std::shared_ptr<Description>, const std::string& newId, Limits = {});
DescriptionCopy unionDescriptions(std::shared_ptr<Description>, std::shared_ptr<Description>, const std::string& newId, Limits = {});
DescriptionCopy intersectDescriptions(std::shared_ptr<Description>, std::shared_ptr<Description>, const std::string& newId, Limits = {});
DescriptionCopy selectDescriptions(std::shared_ptr<Description>, const std::function<bool(const Description&)>&, const std::string& newId, Limits = {});
DescriptionCopy copyWithout(std::shared_ptr<Description>, const Description&, const std::string& newId, Limits = {});
DescriptionCopy copyWithoutAll(std::shared_ptr<Description>, const std::vector<std::shared_ptr<Description>>&, const std::string& newId, Limits = {});
DescriptionCopy collectDescriptions(std::shared_ptr<Description>, const std::function<std::shared_ptr<Description>(std::shared_ptr<Description>)>&, const std::string& newId, Limits = {});
DescriptionCopy copyEmpty(std::shared_ptr<Description>, const std::string& newId, Limits = {});
// Native indices are zero-based half-open, unlike Smalltalk copyFrom:to:.
DescriptionCopy copyRange(std::shared_ptr<Description>, std::size_t begin, std::size_t end, const std::string& newId, Limits = {});
// Compatibility spelling: a fresh snapshot of runtime-registered class names.
std::vector<std::string> portableClasses();
struct AccessorObject {
    std::string id;
    Accessor value; // Leaf state, or className for chain/delegator nodes.
    std::shared_ptr<AccessorObject> accessor, next;
    static std::shared_ptr<AccessorObject> fromAccessor(const std::string&, const Accessor&, Limits = {});
    Accessor snapshot(Limits = {}) const;
};
struct ConditionObject {
    std::string id;
    Predicate predicate;
    bool evaluate(const Value& value, Limits limits = {}) const { return predicate.evaluate(value, limits); }
};
// Explicit session for recursively describing and editing description objects.
// Retains targets; adapters fail after session destruction. Its resolver is local.
class RecursiveDescriptionModel {
    struct State;
    std::shared_ptr<State> state_;
public:
    explicit RecursiveDescriptionModel(Limits = {});
    Graph& resolver();
    std::shared_ptr<Model> bind(std::shared_ptr<Description>);
    std::shared_ptr<Description> describe(std::shared_ptr<Description>);
    std::shared_ptr<Description> describeReflective(std::shared_ptr<Description>);
    std::shared_ptr<Model> bind(std::shared_ptr<AccessorObject>);
    std::shared_ptr<Model> bind(std::shared_ptr<ConditionObject>);
    std::shared_ptr<Description> describe(std::shared_ptr<AccessorObject>);
    std::shared_ptr<Description> describe(std::shared_ptr<ConditionObject>);
    // A memento's self-description is its represented model's exact description.
    std::shared_ptr<Model> bind(const std::string& id, std::shared_ptr<Memento>);
    std::shared_ptr<Description> describe(const std::string& id, std::shared_ptr<Memento>);
    // One finite batch over the input's reachable descriptions, not infinite meta-recursion.
    Graph bootstrap(std::shared_ptr<Description>);
    Graph bootstrap(const std::string& id, std::shared_ptr<Memento>);
    Graph schemas() const;
};
// MAStringReader/MAStringWriter; these do not validate values or apply defaults.
Value readString(const Description&, const std::string&, Limits = {});
std::string writeString(const Description&, const Value&, const Graph* = nullptr, Limits = {});
std::string writeString(const Description&, const Model&, const Graph* = nullptr, Limits = {});
Predicate decodePredicate(const std::string&, Limits = {});
std::string encodePredicate(const Predicate&, Limits = {});
Accessor decodeAccessor(const std::string&, Limits = {});
std::string encodeAccessor(const Accessor&, Limits = {});
Value decodeValue(const std::string&, Limits = {});
std::string encodeValue(const Value&, Limits = {});
std::vector<ValidationError> validateValue(const Description&, const Value&, const Graph* = nullptr, Limits = {});
std::vector<ValidationError> validate(const Description&, const Model&, Limits = {}, const Graph* = nullptr);
// Walk composition and reference edges once; recursive references terminate.
void visit(const Description&, const std::function<void(const Description&)>&, Limits = {});
// Initial bootstrap: the common scalar presentation/validation property descriptions.
std::shared_ptr<Description> descriptionModel();
} // namespace magritte
