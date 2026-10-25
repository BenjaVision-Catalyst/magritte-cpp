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
// This first profile deliberately accepts exact signed 64-bit integers only.
struct Value {
    using Array = std::vector<Value>;
    std::variant<std::monostate, bool, std::int64_t, std::string, Array> data;
    Value() = default;
    explicit Value(bool v) : data(v) {}
    explicit Value(std::int64_t v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(const char* v) : data(std::string(v)) {}
    explicit Value(Array v) : data(std::move(v)) {}
    bool isNil() const { return std::holds_alternative<std::monostate>(data); }
    bool operator==(const Value& v) const { return data == v.data; }
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
struct Model {
    virtual ~Model() = default;
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
    std::vector<Condition> conditions;
    std::function<Value(const void*)> nativeRead;
    std::function<void(void*, const Value&)> nativeWrite;
    std::function<Value()> defaultFunction;
    bool isContainer() const;
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
public:
    // Caller retains ownership and must outlive this adapter and every access.
    explicit NativeModel(void* object) : object_(object) {}
    Value readUsing(const Description&) const override;
    void writeUsing(const Description&, Value) override;
};
struct Graph {
    std::string root;
    std::map<std::string, std::shared_ptr<Description>> descriptions;
    std::map<std::string, AdaptiveModel> objects;
    static Graph decode(const std::string&, Limits = {});
    std::string encode(Limits = {}) const;
};
Value decodeValue(const std::string&, Limits = {});
std::string encodeValue(const Value&, Limits = {});
std::vector<ValidationError> validateValue(const Description&, const Value&);
std::vector<ValidationError> validate(const Description&, const Model&, Limits = {});
// Walk description identities once; cycles/depth/work overruns fail explicitly.
void visit(const Description&, const std::function<void(const Description&)>&, Limits = {});
// Initial bootstrap: the common scalar presentation/validation property descriptions.
std::shared_ptr<Description> descriptionModel();
} // namespace magritte
