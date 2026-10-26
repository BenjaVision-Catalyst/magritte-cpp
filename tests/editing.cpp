#include <algorithm>
#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const std::string &s) {
    if (!b)
        throw Error(s);
    ++checks;
}
template <class F> void rejects(F f, const std::string &s) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, s);
}
J file(const char *name) {
    std::ifstream in(std::string(FIXTURES) + "/" + name);
    check(bool(in), "Missing fixture");
    J j;
    in >> j;
    return j;
}
Value value(const J &j) { return decodeValue(j.dump()); }

J values(const std::map<std::string, Value> &values) {
    J j = J::object();
    for (const auto &p : values)
        j[p.first] = J::parse(encodeValue(p.second));
    return j;
}
J errors(const std::vector<ValidationError> &errors) {
    J j = J::array();
    for (const auto &e : errors)
        j.push_back(J{{"code", e.code}, {"description", e.descriptionId}});
    return j;
}
J snapshot(Memento &m, Graph &g, const J &errors) {
    J reads = J::object(), readErrors = J::array();
    for (const auto &d : m.description()->orderedChildren()) {
        try {
            reads[d->id] = J::parse(encodeValue(m.readUsing(*d)));
        } catch (const std::exception &) {
            readErrors.push_back(d->id);
        }
    }
    J state{{"errors", errors},
            {"reads", reads},
            {"readErrors", readErrors},
            {"stored", values(g.objects.at("model").values)},
            {"peerStored", values(g.objects.at("peerModel").values)}};
    if (auto cached = dynamic_cast<CachedMemento *>(&m)) {
        state["cache"] = values(cached->cache());
        state["hasChanges"] = cached->hasChanges();
    }
    if (auto checked = dynamic_cast<CheckedMemento *>(&m)) {
        state["original"] = values(checked->original());
        state["hasModelChangedElsewhere"] = checked->hasModelChangedElsewhere();
        state["hasConflict"] = checked->hasConflict();
    }
    return state;
}
void replay(const J &reference, const J &scenarios, const char *exportPath) {
    for (const auto &c : reference.at("cases")) {
        auto g = Graph::decode(c.at("graph").dump());
        auto root = g.descriptions.at(g.root);
        auto &model = g.objects.at("model");
        std::unique_ptr<Memento> m;
        if (c.at("memento") == "MAStraightMemento")
            m = std::make_unique<StraightMemento>(model, root, &g);
        else if (c.at("memento") == "MACachedMemento")
            m = std::make_unique<CachedMemento>(model, root, &g);
        else
            m = std::make_unique<CheckedMemento>(model, root, &g);
        auto compare = [&](std::size_t i, const J &errors) {
            auto got = snapshot(*m, g, errors);
            check(got == c.at("snapshots").at(i),
                  "GT editing snapshot: " + c.at("name").get<std::string>() + " " +
                      c.at("memento").get<std::string>() + " step " + std::to_string(i) + "\nexpected " +
                      c.at("snapshots").at(i).dump(2) + "\ngot " + got.dump(2));
        };
        compare(0, J::array());
        const J *scenario = nullptr;
        for (const auto &s : scenarios)
            if (s.at("name") == c.at("name"))
                scenario = &s;
        check(scenario, "Scenario found");
        std::size_t i = 0;
        for (const auto &step : scenario->at("steps")) {
            J result = J::array();
            auto action = step.at("action").get<std::string>();
            try {
                if (action == "write")
                    m->writeUsing(*g.descriptions.at(step.at("field")), value(step.at("value")));
                else if (action == "external")
                    model.writeUsing(*g.descriptions.at(step.at("field")), value(step.at("value")));
                else if (action == "externalObject")
                    g.objects.at("peerModel")
                        .writeUsing(*g.descriptions.at(step.at("field")), value(step.at("value")));
                else if (action == "property")
                    g.descriptions.at(step.at("field"))->properties[step.at("key")] = value(step.at("value"));
                else if (action == "addField")
                    root->children.push_back(g.descriptions.at("extra"));
                else if (action == "removeField") {
                    auto d = g.descriptions.at("name");
                    root->children.erase(std::find(root->children.begin(), root->children.end(), d));
                } else if (action == "reset")
                    m->reset();
                else if (action == "commit")
                    m->commit();
                else if (action == "validate")
                    result = errors(m->validate());
                else
                    throw Error("Unknown scenario action");
            } catch (const std::exception &) {
                result = J::array({J{{"code", "runtimeError"}, {"description", "editor"}}});
            }
            compare(++i, result);
        }
        if (c.at("memento") == "MACheckedMemento" && c.at("name") == "ordinary-edit") {
            auto score = g.descriptions.at("score");
            auto meta = descriptionModelFor(*score);
            CheckedMemento metadata(*score, meta.descriptions.at(meta.root), &meta);
            metadata.writeUsing(*meta.descriptions.at(meta.root + ".min"), Value(std::int64_t(7)));
            check(metadata.validate().empty(), "Edited constraint metadata validates");
            metadata.commit();
            check(m->validate().at(0).code == "MARangeError",
                  "Self-description commit changes the active editor's validation");
            m->writeUsing(*score, Value(std::int64_t(8)));
            check(m->validate().empty(), "Editor adopts changed constraint without recompilation");
            m->commit();
            if (exportPath) {
                std::ofstream out(exportPath);
                out << g.encode();
                check(bool(out), "Edited graph export");
            }
        }
    }
    auto g = Graph::decode(reference.at("cases").front().at("graph").dump());
    for (const auto &c : reference.at("writes"))
        check(g.descriptions.at(c.at("description"))
                      ->shouldWrite(value(c.at("candidate")), value(c.at("previous"))) == c.at("result"),
              "Pinned shouldWrite matrix");
}

void nativeEdges() {
    for (int mode = 0; mode < 3; ++mode) {
        struct State {
            Value value;
            int reads = 0, writes = 0, defaults = 0;
        };
        State state;
        auto root = std::make_shared<Description>();
        root->id = "nativeRoot";
        root->className = "MAContainer";
        auto d = std::make_shared<Description>();
        d->id = "value";
        d->className = "MANumberDescription";
        root->children = {d};
        d->nativeRead = [](const void *p) {
            auto &state = *const_cast<State *>(static_cast<const State *>(p));
            ++state.reads;
            return state.value;
        };
        d->nativeWrite = [](void *p, const Value &v) {
            auto &state = *static_cast<State *>(p);
            ++state.writes;
            state.value = v;
        };
        d->defaultFunction = [&] {
            ++state.defaults;
            return Value(std::int64_t(5));
        };
        NativeModel model(&state);
        std::unique_ptr<Memento> m;
        if (mode == 0)
            m = std::make_unique<StraightMemento>(model, root);
        else if (mode == 1)
            m = std::make_unique<CachedMemento>(model, root);
        else
            m = std::make_unique<CheckedMemento>(model, root);
        check(state.reads == (mode == 0 ? 0 : mode == 1 ? 1 : 2), "Constructor raw pull count");
        check(state.defaults == (mode == 0 ? 0 : 1), "Cooked defaults evaluated once on reset");
        check(m->readUsing(*d) == Value(std::int64_t(5)) && m->readUsing(*d) == Value(std::int64_t(5)),
              "Functional default reads");
        check(state.defaults == (mode == 0 ? 2 : 1),
              "Straight defaults remain lazy; cached defaults are cached");
        m->writeUsing(*d, Value(std::int64_t(8)));
        m->commit();
        check(state.value == Value(std::int64_t(8)) && state.writes == 1,
              "Native model edited through accessor callbacks");
    }
    auto root = std::make_shared<Description>();
    root->id = "root";
    root->className = "MAContainer";
    auto a = std::make_shared<Description>();
    a->id = "a";
    a->key = "a";
    a->properties["default"] = Value("Default");
    auto b = std::make_shared<Description>();
    b->id = "b";
    b->key = "b";
    root->children = {a, b};
    DictionaryModel model;
    model.values["a"] = Value("Default");
    CachedMemento cache(model, root);
    cache.writeUsing(*a, {});
    check(!cache.hasChanges(), "Pinned nil/default symmetry");
    cache.commit();
    check(model.values.at("a") == Value("Default"), "Ignored nil edit does not clear a stored default");
    auto foreign = *a;
    rejects([&] { cache.readUsing(foreign); }, "Foreign description identity");
    auto unknown = std::make_shared<Description>();
    unknown->id = "foreign";
    unknown->key = "foreign";
    rejects([&] { cache.writeUsing(*unknown, Value("x")); }, "Foreign field cache insertion");
    Limits tiny;
    tiny.visits = 1;
    rejects([&] { CachedMemento limited(model, root, nullptr, tiny); }, "Memento traversal bounds");
    root->children.push_back(root);
    rejects([&] { CachedMemento cyclic(model, root); }, "Memento owning cycles");
    root->children.pop_back();
    auto nested = std::make_shared<Description>();
    nested->id = "nested";
    nested->className = "MAContainer";
    root->children.push_back(nested);
    rejects([&] { CachedMemento unsupported(model, root); },
            "Nested owning memento containers fail explicitly");
    root->children.pop_back();
    root->children.push_back(a);
    rejects([&] { CachedMemento duplicate(model, root); }, "Duplicate editing field IDs");
    root->children.pop_back();
    auto replacement = std::make_shared<Description>(*a);
    root->children[0] = replacement;
    rejects([&] { cache.hasChanges(); }, "Replacing descriptor identity requires reset");
    cache.reset();
    check(cache.readUsing(*replacement) == Value("Default"), "Reset adopts replacement descriptor");
    root->children[0] = a;
    cache.reset();
    Limits aggregate;
    aggregate.visits = 100;
    CachedMemento bounded(model, root, nullptr, aggregate);
    bounded.writeUsing(*a, Value(std::string(60, 'x')));
    rejects([&] { bounded.writeUsing(*b, Value(std::string(60, 'y'))); }, "Aggregate editing cache budget");
    check(bounded.readUsing(*b).isNil(), "Rejected oversized edit leaves cache unchanged");
    struct Failure {
        std::map<std::string, Value> data;
    };
    Failure failure;
    for (auto d : {a, b}) {
        auto key = d->id;
        d->nativeRead = [key](const void *p) {
            auto &values = static_cast<const Failure *>(p)->data;
            auto i = values.find(key);
            return i == values.end() ? Value() : i->second;
        };
        d->nativeWrite = [key](void *p, const Value &v) {
            if (key == "b")
                throw Error("write failed");
            static_cast<Failure *>(p)->data[key] = v;
        };
    }
    NativeModel native(&failure);
    CachedMemento partial(native, root);
    partial.writeUsing(*a, Value("First"));
    partial.writeUsing(*b, Value("Second"));
    rejects([&] { partial.commit(); }, "Commit callback failure propagates");
    check(failure.data.at("a") == Value("First") && partial.readUsing(*b) == Value("Second"),
          "Commit is not a rollback transaction and does not reset after failure");
    auto meta = descriptionModelFor(*a);
    auto type = meta.descriptions.at(meta.root + ".type");
    rejects([&] { a->writeUsing(*type, Value("Bogus")); }, "Derived type has no raw setter");
}
int main(int argc, char **argv) {
    try {
        for (const auto &c : file("self-description-reference.json")) {
            Description target;
            target.id = "target";
            target.className = c.at("class");
            target.key = c.at("key");
            for (auto p = c.at("properties").begin(); p != c.at("properties").end(); ++p)
                target.properties[p.key()] = value(p.value());
            for (auto p = c.at("slots").begin(); p != c.at("slots").end(); ++p)
                target.properties[p.key()] = value(p.value());
            auto meta = descriptionModelFor(target);
            auto graph = J::parse(meta.encode());
            check(graph == c.at("graph"),
                  "GT self-description graph: " + target.className + "\n" + graph.dump(2));
            for (const auto &read : c.at("reads"))
                check(target.readUsing(*meta.descriptions.at(read.at("field"))) == value(read.at("value")),
                      "GT metadata read " + read.at("field").get<std::string>() + " expected " +
                          read.at("value").dump() + " got " +
                          encodeValue(target.readUsing(*meta.descriptions.at(read.at("field")))));
            check(validate(*meta.descriptions.at(meta.root), target, {}, &meta).empty(),
                  "GT metadata validation");
            CheckedMemento editor(target, meta.descriptions.at(meta.root), &meta);
            for (const auto &edit : c.at("edits"))
                editor.writeUsing(*meta.descriptions.at(edit.at("field")), value(edit.at("value")));
            check(target.property("label") == Value("Custom"), "Metadata staged without altering target");
            for (const auto &read : c.at("stagedReads"))
                check(editor.readUsing(*meta.descriptions.at(read.at("field"))) == value(read.at("value")),
                      "GT staged metadata read");
            check(errors(editor.validate()) == c.at("editValidation"), "GT staged metadata validation");
            editor.commit();
            for (const auto &read : c.at("finalReads"))
                check(target.readUsing(*meta.descriptions.at(read.at("field"))) == value(read.at("value")),
                      "GT committed metadata read");
            if (target.className == "MANumberDescription") {
                J result = J::array();
                for (const auto &e : validateValue(target, Value(std::int64_t(5))))
                    result.push_back(e.code);
                check(result == c.at("sampleValidation"),
                      "Self-edited constraint changes native value validation");
            }
        }
        nativeEdges();
        replay(file("editing-reference.json"), file("editing-scenarios.json"), argc > 1 ? argv[1] : nullptr);
        std::cout << checks << " editing checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
