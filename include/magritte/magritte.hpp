#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace magritte {
// The portable numeric domain deliberately accepts exact signed 64-bit integers only.
struct Value {
    struct Reference { std::string id; bool operator==(const Reference& r) const { return id == r.id; } };
    struct Character { std::string utf8; bool operator==(const Character& c) const { return utf8 == c.utf8; } };
    struct Collection { std::string kind; std::vector<Value> elements; };
    using Array = std::vector<Value>;
    std::variant<std::monostate, bool, std::int64_t, std::string, Array, Reference, Character, Collection> data;
    Value() = default;
    explicit Value(bool v) : data(v) {}
    explicit Value(std::int64_t v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(const char* v) : data(std::string(v)) {}
    explicit Value(Array v) : data(std::move(v)) {}
    explicit Value(Reference v) : data(std::move(v)) {}
    explicit Value(Character v) : data(std::move(v)) {}
    explicit Value(Collection v) : data(std::move(v)) {}
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
struct ValidationError {
    std::string descriptionId, code, message;
};
struct Description;
struct Graph;
struct Model {
    virtual ~Model() = default;
    virtual bool kindOf(const std::string& name) const { return name == "Object"; }
    virtual Value readUsing(const Description&) const = 0;
    // Raw model writes, like Smalltalk's write:using:, do not validate or enforce UI flags.
    virtual void writeUsing(const Description&, Value) = 0;
};
struct Condition {
    std::string label;
    std::function<bool(const Value&)> test;
};
struct Description : Model {
    std::string id, className = "MAStringDescription", key;
    std::map<std::string, Value> properties;
    std::vector<std::shared_ptr<Description>> children;
    // Graph owns descriptions. Non-owning reference edges permit recursive models.
    std::weak_ptr<Description> reference;
    std::vector<std::string> classes;
    std::vector<Value> options;
    std::vector<std::pair<Value, std::string>> labels;
    std::vector<Condition> conditions;
    std::function<Value(const void*)> nativeRead;
    std::function<void(void*, const Value&)> nativeWrite;
    std::function<Value()> defaultFunction;
    bool isContainer() const;
    bool isRelation() const;
    bool isOption() const;
    bool isReference() const { return isRelation() || isOption(); }
    bool isMultiple() const;
    std::shared_ptr<Description> referenceDescription() const;
    std::vector<Value> allOptions(Limits = {}) const;
    std::vector<Value> allOptionsWith(const Value&, Limits = {}) const;
    std::string labelForOption(const Value&, Limits = {}) const;
    Value property(const std::string&) const;
    Value defaultValue() const;
    bool flag(const std::string&) const;
    std::int64_t priority() const;
    std::vector<std::shared_ptr<Description>> orderedChildren() const;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct AdaptiveModel : Model {
    std::string id;
    bool kindOf(const std::string& name) const override { return name == "Object" || name == "MABasicObject" || name == "MAAdaptiveModel"; }
    std::shared_ptr<Description> description;
    // Keys are stable graph description identities, not field names.
    std::map<std::string, Value> values;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct DictionaryModel : Model {
    std::map<std::string, Value> values;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
class NativeModel : public Model {
    void* object_;
    std::vector<std::string> classes_;
public:
    // Caller retains ownership and must outlive this adapter and every access.
    explicit NativeModel(void* object, std::vector<std::string> classes = {}) : object_(object), classes_(std::move(classes)) {}
    bool kindOf(const std::string&) const override;
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct Graph {
    std::string format = "magritte.graph/2", root;
    // Local native targets are retained adapters, never portable graph data.
    std::map<std::string, std::shared_ptr<Model>> nativeObjects;
    const Model& resolve(const Value::Reference&) const;
    const Model& resolve(const Value&) const;
    std::map<std::string, std::shared_ptr<Description>> descriptions;
    std::map<std::string, AdaptiveModel> objects;
    static Graph decode(const std::string&, Limits = {});
    std::string encode(Limits = {}) const;
};
Value decodeValue(const std::string&, Limits = {});
std::string encodeValue(const Value&, Limits = {});
std::vector<ValidationError> validateValue(const Description&, const Value&, const Graph* = nullptr, Limits = {});
std::vector<ValidationError> validate(const Description&, const Model&, Limits = {}, const Graph* = nullptr);
// Walk composition and reference edges once; recursive references terminate.
void visit(const Description&, const std::function<void(const Description&)>&, Limits = {});
// Initial bootstrap: the common scalar presentation/validation property descriptions.
std::shared_ptr<Description> descriptionModel();
} // namespace magritte
