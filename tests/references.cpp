#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Reference mismatch") {
    ++checks;
    if (!ok)
        throw Error(message);
}
template <class F> void fails(F f) {
    bool caught = false;
    try {
        f();
    } catch (const Error &) {
        caught = true;
    }
    check(caught, "Expected reference failure");
}
std::shared_ptr<Description> node(const std::string &id,
                                  const std::string &cls = "MAStringDescription") {
    auto d = std::make_shared<Description>();
    d->id = d->key = id;
    d->className = cls;
    return d;
}
std::shared_ptr<DisplayBlock> renderer() {
    return std::make_shared<DisplayBlock>(
        DisplayBlock::Operation([](const Value &v, const Description &d) {
            return std::get<std::string>(d.property("label").data) + ":" +
                   std::get<std::string>(v.data);
        }));
}
Graph graphOf(std::shared_ptr<Description> owner, std::shared_ptr<Description> ref) {
    auto root = node("root", "MAContainer");
    root->children = {owner};
    Graph g;
    g.format = "magritte.graph/4";
    g.root = root->id;
    g.descriptions = {{root->id, root}, {owner->id, owner}, {ref->id, ref}};
    return g;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/reference-access-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("display")) {
            auto owner = node("owner", row.at("className")), reference = node("reference");
            owner->reference = reference;
            owner->properties["label"] = Value("Owner");
            reference->properties["label"] = Value("Reference");
            auto old =
                std::make_shared<DisplayBlock>(std::function<std::string()>([] { return "old"; }));
            reference->display(old);
            auto style = row.at("ownerDisplay").get<std::string>();
            if (style == "asString")
                owner->display("asString");
            else if (style == "nil")
                owner->properties["displayBlockOrSymbol"] = Value();
            else if (style == "block")
                owner->display(renderer());
            // Structural visits and copies must not send an imperative reference getter.
            visit(*owner, [](const Description &) {});
            check(reference->displayBlock == old);
            auto copy = copyDescription(owner, "copy");
            check(reference->displayBlock == old &&
                  copy.root->reference.lock()->displayBlock == old);
            check(owner->referenceDescription() == reference);
            if (style == "block")
                check(reference->displayBlock == owner->displayBlock);
            else
                check(!reference->displayBlock);
            if (row.at("result").is_null())
                fails([&] { writeString(*reference, Value("Ada")); });
            else
                check(writeString(*reference, Value("Ada")) == row.at("result"));
            check(owner->referenceDescription() == reference);
            if (style == "absent")
                check(reference->properties.count("displayBlockOrSymbol") == 0);
        }
        for (const auto &row : fixture.at("undefined")) {
            auto owner = node("owner", row.at("className")), reference = node("reference");
            owner->reference = reference;
            reference->properties["undefined"] = Value("reference-before");
            owner->display("asString");
            reference->display("yourself");
            auto value =
                row.at("value").is_null() ? Value() : Value(row.at("value").get<std::string>());
            if (row.at("route") == "direct")
                check(&owner->setUndefined(value) == owner.get());
            else
                owner->writeSlot("MASelectorAccessor", "undefined:", value);
            check(owner->property("undefined") == value);
            auto expected = row.at("targetValue").is_null()
                                ? Value()
                                : Value(row.at("targetValue").get<std::string>());
            check(reference->property("undefined") == expected);
            check(reference->property("displayBlockOrSymbol") ==
                  Value(Value::Symbol{owner->isOption() ? "asString" : "yourself"}));
        }
        auto owner = node("owner", "MASingleOptionDescription"), reference = node("reference");
        owner->reference = reference;
        owner->display("asString");
        reference->display("yourself");
        auto graph = graphOf(owner, reference);
        auto wire = graph.encode();
        check(reference->property("displayBlockOrSymbol") == Value(Value::Symbol{"yourself"}));
        auto imported = Graph::decode(wire);
        check(imported.descriptions.at("reference")->property("displayBlockOrSymbol") ==
              Value(Value::Symbol{"yourself"}));
        check(imported.encode() == wire);
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        // Reading one shared reference changes it in place; another owner's configuration survives.
        auto other = node("other", "MATokenDescription");
        other->reference = reference;
        other->display(renderer());
        reference->properties["label"] = Value("Shared");
        other->referenceDescription();
        check(writeString(*reference, Value("Ada")) == "Shared:Ada");
        owner->referenceDescription();
        check(!reference->displayBlock && other->displayBlock != nullptr);
        // Validation touches the getter only when an extensible nonmember actually uses the
        // reference.
        auto old = renderer();
        reference->display(old);
        fails([&] { graph.encode(); });
        check(reference->displayBlock == old);
        owner->options = {Value("Ada")};
        check(validateValue(*owner, Value("Ada")).empty());
        check(reference->displayBlock == old);
        owner->properties["extensible"] = Value(true);
        check(validateValue(*owner, Value("Bob")).empty());
        check(!reference->displayBlock);
        // Token/to-many visits refresh propagation per element, while empty collections do not read
        // reference.
        for (const auto &cls :
             std::vector<std::string>{"MATokenDescription", "MAToManyRelationDescription",
                                      "MAToManyScalarRelationDescription"}) {
            auto parent = node("parent", cls), target = node("target");
            parent->reference = target;
            auto second = std::make_shared<DisplayBlock>(
                std::function<std::string()>([] { return "second"; }));
            parent->display(std::make_shared<DisplayBlock>(std::function<std::string()>([&] {
                parent->display(second);
                return "first";
            })));
            check(writeString(*parent, Value(Value::Array{Value("a"), Value("b")})) ==
                  (cls == "MATokenDescription" ? "first second" : "first, second"));
            target->display("yourself");
            check(writeString(*parent, Value(Value::Array{})).empty());
            check(target->property("displayBlockOrSymbol") == Value(Value::Symbol{"yourself"}));
        }
        // Option sorting's singleton/empty paths never invoke the reference getter.
        owner->properties["sorted"] = Value(true);
        owner->properties["required"] = Value(true);
        owner->options = {Value("Ada")};
        reference->display("yourself");
        owner->allOptions();
        check(reference->property("displayBlockOrSymbol") == Value(Value::Symbol{"yourself"}));
        auto nested = node("nested", "MAMultipleOptionDescription"), leaf = node("leaf");
        owner->reference = nested;
        nested->reference = leaf;
        owner->setUndefined(Value("missing"));
        check(nested->property("undefined") == Value("missing") &&
              leaf->property("undefined") == Value("missing"));
        owner->reference = owner;
        Limits low;
        low.depth = 2;
        fails([&] { owner->setUndefined(Value("partial"), low); });
        check(owner->property("undefined") == Value("partial"));
        owner->reference = reference;
        fails([&] { owner->setUndefined(Value(true)); });
        owner->reference.reset();
        auto generated = owner->referenceDescription();
        check(generated->className == "MAStringDescription");
        owner->setUndefined(Value("saved"));
        check(generated->property("undefined") == Value("saved"));
        check(owner->property("undefined") == Value("saved"));
        std::cout << checks << " reference checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
