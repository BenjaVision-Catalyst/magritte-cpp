#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Message mismatch") {
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
    check(caught, "Expected explicit message failure");
}
Value val(const J &j) { return decodeValue(j.dump()); }
J errors(const std::vector<ValidationError> &es) {
    J result = J::array();
    for (const auto &e : es)
        result.push_back(J{{"code", e.code}, {"message", e.message}});
    return result;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::ifstream in(std::string(FIXTURES) + "/messages-scenarios.json");
        J fixture;
        in >> fixture;
        for (const auto &row : fixture.at("getters")) {
            Description d;
            d.id = "sample";
            d.className = row.at("className");
            for (auto p = row.at("properties").begin(); p != row.at("properties").end(); ++p)
                d.properties[p.key()] = val(p.value());
            auto key = row.at("key").get<std::string>();
            auto before = d.properties;
            check(d.property(key) == val(row.at("result")));
            check(d.readSlot("MASelectorAccessor", key) == val(row.at("result")));
            check(d.properties == before, "Fallback must not store generated text");
        }
        for (const auto &row : fixture.at("validations")) {
            Description d;
            d.id = "sample";
            d.className = row.at("className");
            for (auto p = row.at("properties").begin(); p != row.at("properties").end(); ++p)
                d.properties[p.key()] = val(p.value());
            auto reference = std::make_shared<Description>();
            reference->id = "reference";
            if (d.isReference())
                d.reference = reference;
            if (d.isRelation())
                d.classes = {"String"};
            auto es = validateValue(d, val(row.at("value")));
            check(errors(es) == row.at("errors"));
            for (const auto &e : es)
                check(e.descriptionId == d.id);
        }
        // Generated text uses the configured writer, performs ordered reads, and is not cached.
        Description number;
        number.id = "number";
        number.key = "number";
        number.className = "MANumberDescription";
        number.properties = {{"min", Value(std::int64_t(2))}, {"max", Value(std::int64_t(8))}};
        std::vector<Value> trace;
        number.display(std::make_shared<DisplayBlock>([&](const Value &v) {
            trace.push_back(v);
            return "bound";
        }));
        check(number.rangeErrorMessage() == Value("Input must be between bound and bound"));
        check(trace == std::vector<Value>({Value(std::int64_t(2)), Value(std::int64_t(8))}));
        number.setRangeErrorMessage(Value("override"));
        trace.clear();
        check(number.rangeErrorMessage() == Value("override") && trace.empty());
        number.properties.erase("rangeErrorMessage");
        number.display(std::make_shared<DisplayBlock>([&](const Value &v) {
            if (trace.empty())
                number.properties["max"] = Value(std::int64_t(9));
            trace.push_back(v);
            return "b";
        }));
        check(number.rangeErrorMessage() == Value("Input must be between b and b"));
        check(trace == std::vector<Value>({Value(std::int64_t(2)), Value(std::int64_t(9))}));
        trace.clear();
        number.display(std::make_shared<DisplayBlock>([&](const Value &) {
            number.properties["min"] = Value();
            number.properties["max"] = Value();
            return "b";
        }));
        check(number.rangeErrorMessage().isNil(), "Presence is inspected after both writer calls");
        number.display(std::make_shared<DisplayBlock>([] {
            throw Error("writer");
            return std::string();
        }));
        number.properties["min"] = Value(std::int64_t(2));
        fails([&] { number.rangeErrorMessage(); });
        number.setRangeErrorMessage(Value("safe"));
        check(number.rangeErrorMessage() == Value("safe"));
        // Nil overrides remain nil data. Emitting nil as a String leaf fails rather than inventing
        // text.
        Description text;
        text.id = "text";
        text.key = "text";
        check(&text.setRequiredErrorMessage(Value("required")) == &text);
        check(&text.setKindErrorMessage(Value("kind")) == &text);
        check(&text.setConflictErrorMessage(Value("conflict")) == &text);
        text.setKindErrorMessage(Value());
        check(text.kindErrorMessage().isNil());
        fails([&] { validateValue(text, Value(true)); });
        check(validateValue(text, Value("ok")).empty());
        text.properties["visible"] = Value(false);
        check(validateValue(text, Value(true)).empty());
        text.properties.erase("visible");
        fails([&] { text.setKindErrorMessage(Value(true)); });
        check(text.kindErrorMessage().isNil());
        fails([&] { text.rangeErrorMessage(); });
        fails([&] { text.setRangeErrorMessage(Value("x")); });
        // Condition labels stay independent; extensible options use the failing child description's
        // text.
        text.properties.erase("kindErrorMessage");
        text.conditions.push_back({"condition", [](const Value &) { return false; }});
        auto es = validateValue(text, Value("ok"));
        check(es.size() == 1 && es[0].message == "condition");
        auto option = std::make_shared<Description>(), child = std::make_shared<Description>();
        option->id = "option";
        option->className = "MASingleOptionDescription";
        child->id = "child";
        child->className = "MANumberDescription";
        child->setKindErrorMessage(Value("child message"));
        option->reference = child;
        option->properties["extensible"] = Value(true);
        option->setKindErrorMessage(Value("parent message"));
        es = validateValue(*option, Value("bad"));
        check(es.size() == 1 && es[0].descriptionId == child->id &&
              es[0].message == "child message");
        // Checked conflicts select container text after destructive reset.
        auto root = std::make_shared<Description>(), field = std::make_shared<Description>();
        root->id = "root";
        root->className = "MAContainer";
        field->id = field->key = "data";
        root->children = {field};
        AdaptiveModel model;
        model.description = root;
        model.values[field->id] = Value("old");
        CheckedMemento editor(model, root);
        editor.writeUsing(*field, Value("local"));
        model.values[field->id] = Value("remote");
        root->setConflictErrorMessage(Value("concurrent"));
        es = editor.validate();
        check(es.size() == 1 && es[0].descriptionId == root->id && es[0].message == "concurrent");
        check(editor.readUsing(*field) == Value("remote") && !editor.hasChanges());
        root->setConflictErrorMessage(Value());
        editor.writeUsing(*field, Value("local2"));
        model.values[field->id] = Value("remote2");
        fails([&] { editor.validate(); });
        check(editor.readUsing(*field) == Value("remote2") && !editor.hasChanges());
        // Metadata selectors, independent copies, legacy gates and portable snapshots.
        root->setConflictErrorMessage(Value("concurrent"));
        field->writeSlot("MASelectorAccessor", "requiredErrorMessage:", Value("required"));
        field->writeSlot("MASelectorAccessor", "kindErrorMessage:", Value("kind"));
        auto copy = copyDescription(field, "copy");
        copy.root->setKindErrorMessage(Value("copied"));
        check(field->kindErrorMessage() == Value("kind"));
        auto numeric = std::make_shared<Description>();
        numeric->id = numeric->key = "score";
        numeric->className = "MANumberDescription";
        numeric->writeSlot("MASelectorAccessor", "rangeErrorMessage:", Value("range"));
        root->children.push_back(numeric);
        Graph g;
        g.format = "magritte.graph/4";
        g.root = root->id;
        g.descriptions = {{root->id, root}, {field->id, field}, {numeric->id, numeric}};
        auto wire = g.encode();
        auto imported = Graph::decode(wire);
        check(imported.encode() == wire);
        check(imported.descriptions.at(numeric->id)->rangeErrorMessage() == Value("range"));
        if (argc > 1)
            std::ofstream(argv[1]) << wire;
        g.format = "magritte.graph/3";
        check(Graph::decode(g.encode()).descriptions.at(root->id)->conflictErrorMessage() ==
              Value("concurrent"));
        g.format = "magritte.graph/2";
        fails([&] { g.encode(); });
        field->properties["kindErrorMessage"] = Value(false);
        g.format = "magritte.graph/4";
        fails([&] { g.encode(); });
        field->properties["kindErrorMessage"] = Value();
        check(Graph::decode(g.encode()).descriptions.at(field->id)->kindErrorMessage().isNil());
        Limits low;
        low.bytes = 1;
        fails([&] { field->requiredErrorMessage(low); });
        fails([&] { field->setRequiredErrorMessage(Value("too long"), low); });
        check(field->requiredErrorMessage() == Value("required"));
        std::cout << checks << " message checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
