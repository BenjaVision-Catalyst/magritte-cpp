#include <algorithm>
#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool ok, const std::string &message) {
    if (!ok)
        throw Error(message);
    ++checks;
}
template <class F> void rejects(F f, const std::string &message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, message);
}
J fixture(const std::string &name) {
    std::ifstream in(std::string(FIXTURES) + "/" + name + ".json");
    J j;
    in >> j;
    return j;
}
J canonical(J j) {
    for (auto &d : j["descriptions"])
        if (d.contains("classes"))
            std::sort(d["classes"].begin(), d["classes"].end());
    return j;
}
Value v(const J &j) { return decodeValue(j.dump()); }
struct Example {
    std::shared_ptr<Description> app = std::make_shared<Description>(),
                                 score = std::make_shared<Description>(),
                                 link = std::make_shared<Description>();
    Example() {
        app->id = "app";
        app->className = "MAContainer";
        app->properties["label"] = Value("App");
        score->id = "score";
        score->className = "MANumberDescription";
        score->key = "score";
        score->properties = {{"label", Value("score")},
                             {"min", Value(std::int64_t(0))},
                             {"max", Value(std::int64_t(10))},
                             {"required", Value(true)},
                             {"default", Value(std::int64_t(2))}};
        link->id = "link";
        link->className = "MAToOneRelationDescription";
        link->key = "link";
        link->reference = app;
        link->classes = {"MAMemento"};
        link->properties["label"] = Value("link");
        app->children = {score, link};
    }
    AdaptiveModel model() {
        AdaptiveModel m;
        m.id = "sample";
        m.description = app;
        m.values[score->id] = Value(std::int64_t(3));
        return m;
    }
};
std::shared_ptr<Memento> editorFor(const std::string &cls, Model &model, std::shared_ptr<Description> d,
                                   const Graph *g = nullptr) {
    if (cls == "MAStraightMemento")
        return std::make_shared<StraightMemento>(model, d, g);
    if (cls == "MACachedMemento")
        return std::make_shared<CachedMemento>(model, d, g);
    return std::make_shared<CheckedMemento>(model, d, g);
}
J snapshot(Memento &editor, Model &model, const Example &e, const std::string &phase) {
    J j{{"phase", phase},
        {"value", J::parse(encodeValue(editor.readUsing(*e.score)))},
        {"model", J::parse(encodeValue(model.readUsing(*e.score)))},
        {"sameDescription", editor.description() == e.app},
        {"sameModel", &editor.model() == &model}};
    if (auto cached = dynamic_cast<CachedMemento *>(&editor))
        j["changes"] = cached->hasChanges();
    if (auto checked = dynamic_cast<CheckedMemento *>(&editor)) {
        j["conflict"] = checked->hasConflict();
        j["elsewhere"] = checked->hasModelChangedElsewhere();
    }
    return j;
}
void reference(const char *output) {
    auto f = fixture("bootstrap-reference");
    for(const auto& name:f.at("registry"))check(ClassRegistry::global().find(name).has_value(),"GT class is available");
    for (const auto &row : f.at("mementos")) {
        Example e;
        auto model = e.model();
        auto editor = editorFor(row.at("class"), model, e.app);
        RecursiveDescriptionModel session;
        check(session.bind("editor", editor).get() == editor.get(), "Bind preserves actual memento identity");
        check(session.describe("editor", editor) == e.app, "Memento's exact model description");
        for (const auto &kind : row.at("kinds"))
            check(editor->kindOf(kind.at("class")) == kind.at("result"), "GT memento ancestry");
        auto verify = [&](std::size_t index, const std::string &phase) {
            check(snapshot(*editor, model, e, phase) == row.at("snapshots").at(index),
                  "GT nested memento state " + phase);
        };
        verify(0, "initial");
        editor->writeUsing(*e.score, Value(std::int64_t(5)));
        verify(1, "inner-write");
        CheckedMemento outer(*session.bind("editor", editor), session.describe("editor", editor),
                             &session.resolver());
        outer.writeUsing(*e.score, Value(std::int64_t(8)));
        check(outer.validate().empty(), "Outer editor validates");
        outer.commit();
        verify(2, "outer-commit");
        check(editor->validate().empty(), "Inner editor validates");
        editor->commit();
        verify(3, "inner-commit");
        model.writeUsing(*e.score, Value(std::int64_t(6)));
        verify(4, "external-write");
        std::vector<std::string> errors;
        for (const auto &error : editor->validate())
            errors.push_back(error.code);
        check(J(errors) == row.at("validation"), "GT nested conflict validation");
        verify(5, "validate");
        editor->reset();
        verify(6, "reset");
        check(validateValue(*e.link, Value(Value::Reference{"editor"}), &session.resolver()).empty(),
              "Relation resolves an actual memento");
        rejects([&] { session.resolver().encode(); }, "Editing state remains local");
    }
    Example e;
    RecursiveDescriptionModel session;
    auto batch = session.bootstrap(e.app);
    check(canonical(J::parse(batch.encode())) == canonical(f.at("bootstrap")),
          "GT reachable recursive bootstrap graph");
    check(batch.descriptions.size() == 74, "Finite reachability terminates on reference cycle");
    for (const auto &row : f.at("schemas")) {
        auto schema = session.resolver().descriptions.at(row.at("graph").at("root"));
        auto target = schema->id == "bootstrap.1.app"     ? e.app
                      : schema->id == "bootstrap.2.score" ? e.score
                                                          : e.link;
        auto binding = session.bind(target);
        for (const auto &read : row.at("reads"))
            check(binding->readUsing(*session.resolver().descriptions.at(read.at("field"))) ==
                      v(read.at("value")),
                  "GT bootstrap metadata getter " + read.at("field").get<std::string>() + " expected " +
                      read.at("value").dump() + " got " +
                      encodeValue(binding->readUsing(*session.resolver().descriptions.at(read.at("field")))));
        check(validate(*schema, *binding, {}, &session.resolver()).empty(),
              "GT bootstrap metadata validation");
    }
    auto scoreSchema = batch.descriptions.at("bootstrap.2.score");
    auto target = session.bind(e.score);
    auto constraints = std::make_shared<CheckedMemento>(*target, scoreSchema, &session.resolver());
    check(session.describe("constraints", constraints) == scoreSchema,
          "Recursive memento schema is an existing metadata object");
    CheckedMemento outer(*session.bind("constraints", constraints), scoreSchema, &session.resolver());
    auto minimum = batch.descriptions.at("bootstrap.2.score.min");
    outer.writeUsing(*minimum, Value(std::int64_t(7)));
    check(outer.validate().empty(), "Nested metadata memento validates");
    outer.commit();
    check(e.score->property("min") == Value(std::int64_t(0)), "Outer commit only stages inner metadata");
    check(constraints->readUsing(*minimum) == v(f.at("metadataEdit").at("stagedMin")), "GT staged metadata");
    check(constraints->validate().empty(), "Metadata commit validates");
    constraints->commit();
    check(e.score->property("min") == v(f.at("metadataEdit").at("committedMin")), "GT final constraint edit");
    auto model = e.model();
    model.values["score"] = Value(std::int64_t(9));
    auto appEditor = std::make_shared<CheckedMemento>(model, e.app, &session.resolver());
    check(session.describe("app-editor", appEditor) == e.app, "Publish original model schema");
    auto notes = std::make_shared<Description>();
    notes->id = "notes";
    notes->key = "notes";
    e.app->children.push_back(notes);
    appEditor->reset();
    auto graph = session.schemas();
    check(graph.descriptions.at("notes") == notes,
          "Published self-description exports its current reachable closure");
    graph.root = "app";
    graph.objects.emplace(model.id, model);
    rejects([&] { graph.encode(); }, "Live reflective accessor objects require explicit snapshots");
    for (auto d : {e.app, e.score, e.link})
        d->materializeAccessor();
    auto wire = graph.encode();
    auto decoded = Graph::decode(wire);
    check(decoded.encode() == wire, "Recursive bootstrap fixed point");
    check(validate(*decoded.descriptions.at("app"), decoded.objects.at("sample"), {}, &decoded).empty(),
          "Imported constraint governs application object");
    if (output) {
        std::ofstream out(output);
        out << wire;
        check(bool(out), "Bootstrap export");
    }
    for (const auto &cls : f.at("classValues"))
        check(J::parse(encodeValue(v(cls))) == cls, "Memento class value round trip");
}
void boundaries() {
    Example e;
    auto model = e.model();
    auto editor = std::make_shared<CheckedMemento>(model, e.app);
    RecursiveDescriptionModel session;
    rejects([&] { session.bind("app", editor); }, "Memento ID cannot collide with owned schema");
    check(session.resolver().nativeObjects.empty(), "Rejected bind is atomic");
    session.bind("editor", editor);
    rejects([&] { session.bind("other", editor); }, "A memento has one session identity");
    auto other = std::make_shared<CheckedMemento>(model, e.app);
    rejects([&] { session.bind("editor", other); }, "Distinct editors cannot share identity");
    rejects([&] { session.bind("nil", std::shared_ptr<Memento>{}); }, "Null editor rejected");
    Limits tiny;
    tiny.nodes = 60;
    RecursiveDescriptionModel bounded(tiny);
    bounded.bind(e.app);
    rejects([&] { bounded.bootstrap(e.app); }, "Whole batch respects node bounds");
    check(bounded.schemas().descriptions.empty(), "Failed bootstrap publishes no partial schema");
    RecursiveDescriptionModel collision;
    auto reserved = std::make_shared<Description>();
    reserved->id = "bootstrap.1.app";
    collision.bind(reserved);
    rejects([&] { collision.bootstrap(e.app); }, "Generated batch ID collision");
    check(collision.schemas().descriptions.empty(), "Collision leaves generation unpublished");
    Limits editorLimit;
    editorLimit.nodes = 3;
    RecursiveDescriptionModel boundedEditor(editorLimit);
    rejects([&] { boundedEditor.bind("editor", editor); }, "Editor and schema share aggregate node bound");
    check(boundedEditor.resolver().nativeObjects.empty(), "Over-budget editor bind publishes nothing");
    Limits bytes;
    bytes.bytes = 20000;
    RecursiveDescriptionModel boundedBytes(bytes);
    rejects([&] { boundedBytes.bootstrap(e.app); }, "Whole bootstrap batch has a byte bound");
    check(boundedBytes.schemas().descriptions.empty(), "Over-budget byte batch publishes nothing");
    check(boundedBytes.describeReflective(e.score)->id == "reflective.1.score",
          "Failed batch does not advance generation");
    session.describe("editor", editor);
    e.app->children.push_back(e.app);
    rejects([&] { session.schemas(); }, "Published schema detects later owning cycles");
    e.app->children.pop_back();
    auto duplicate = std::make_shared<Description>();
    duplicate->id = "score";
    e.app->children.push_back(duplicate);
    rejects([&] { session.schemas(); }, "Published schema rejects later identity collisions");
    e.app->children.pop_back();
    auto first = session.bootstrap("editor", editor);
    auto second = session.bootstrap(e.app);
    check(first.root != second.root, "Fresh explicit bootstrap generations");
    check(first.descriptions.size() == second.descriptions.size(),
          "Recursion does not eagerly expand generated metadata");
    auto newMeta = session.describeReflective(first.descriptions.at(first.root));
    check(newMeta->isContainer(), "Generated bootstrap schemas can themselves be described");
}
int main(int argc, char **argv) {
    try {
        reference(argc > 1 ? argv[1] : nullptr);
        boundaries();
        std::cout << checks << " bootstrap checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
