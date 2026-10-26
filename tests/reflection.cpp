#include "historical.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const std::string &m) {
    if (!b)
        throw Error(m);
    ++checks;
}
template <class F> void rejects(F f, const std::string &m) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, m);
}
J fixture(const std::string &n) {
    std::ifstream in(std::string(FIXTURES) + "/" + n + ".json");
    J r;
    in >> r;
    return r;
}
// Historical GT snapshots have an explicit class environment; registrations remain global.
J projected(J names) {
    static const auto available = fixture("reflection-reference").at("registry");
    J result = J::array();
    for (const auto& name : names)
        if (std::find(available.begin(), available.end(), name) != available.end()) result.push_back(name);
    return result;
}
J canonical(J j) {
    omitHistoricalLabelDefaults(j);
    for (auto& d : j["descriptions"]) {
        if (d.contains("classes")) {
            d["classes"] = projected(d["classes"]);
            std::sort(d["classes"].begin(),d["classes"].end());
        }
        if (d.contains("options")) {
            J options = J::array();
            for(const auto& option : d["options"])
                if(option.at("type")!="class" || !projected(J::array({option.at("value")})).empty()) options.push_back(option);
            d["options"] = options;
        }
    }
    return j;
}
Value value(const J &j) { return decodeValue(j.dump()); }
std::shared_ptr<Description> prototype(const J &row, const std::string &id = "target") {
    auto d = std::make_shared<Description>();
    d->id = id;
    d->className = row.at("class");
    d->key = "Value";
    Accessor a;
    a.name = "Value";
    d->accessor = a;
    for (auto p = row.at("properties").begin(); p != row.at("properties").end(); ++p)
        d->properties[p.key()] = value(p.value());
    if (row.contains("separators"))
        d->properties["separators"] = Value(row.at("separators").get<std::string>());
    return d;
}
std::shared_ptr<Description> refFor(const std::shared_ptr<Description> &d) {
    auto r = std::make_shared<Description>();
    r->id = d->id + ".reference";
    if (d->isRelation())
        r->className = "MAContainer";
    else {
        Accessor a;
        a.className = "MANullAccessor";
        r->accessor = a;
    }
    if (d->isReference())
        d->reference = r;
    return r;
}
void compare(RecursiveDescriptionModel &ctx, const std::shared_ptr<Model> &model,
             const std::shared_ptr<Description> &schema, const J &row) {
    Graph g;
    g.format = "magritte.graph/4";
    g.root = schema->id;
    for (const auto &n : row.at("graph").at("descriptions")) {
        auto id = n.at("id").get<std::string>();
        g.descriptions.emplace(id, ctx.resolver().descriptions.at(id));
    }
    auto got = canonical(J::parse(g.encode())), expected = canonical(row.at("graph"));
    check(got == expected, "GT reflection schema " + row.at("class").get<std::string>() + "\n" +
                               J::diff(expected, got).dump(2));
    for (const auto &r : row.at("reads")) {
        auto v = model->readUsing(*ctx.resolver().descriptions.at(r.at("field")));
        check(v == value(r.at("value")), "GT reflective getter " + r.at("field").get<std::string>() +
                                             " got " + encodeValue(v) + " expected " + r.at("value").dump());
    }
    std::vector<std::string> codes;
    for (const auto &e : validate(*schema, *model, {}, &ctx.resolver()))
        codes.push_back(e.code);
    check(J(codes) == row.at("validation"), "GT reflective validation");
}
void reference(const char *output) {
    auto f = fixture("reflection-reference");
    for(const auto& name:f.at("registry"))check(ClassRegistry::global().find(name).has_value(),"GT class is available");
    Description cls;
    cls.id = "class";
    cls.className = "MAClassDescription";
    for (const auto &row : f.at("classValidation")) {
        std::vector<std::string> codes;
        for (const auto &e : validateValue(cls, value(row.at("input"))))
            codes.push_back(e.code);
        check(J(codes) == row.at("errors"), "GT Class validation");
    }
    for (const auto &row : f.at("classReads")) {
        if (row.contains("error"))
            rejects([&] { readString(cls, row.at("text")); }, "GT unsupported class text read");
        else
            check(readString(cls, row.at("text")) == value(row.at("value")), "GT empty class read");
    }
    for (const auto &row : f.at("classWrites")) {
        if (row.contains("error"))
            rejects([&] { writeString(cls, value(row.at("input"))); },
                    "GT class display rejects incompatible values");
        else
            check(writeString(cls, value(row.at("input"))) == row.at("text"), "GT class display");
    }
    for (const auto &row : f.at("descriptions")) {
        auto d = prototype(row);
        auto r = refFor(d);
        RecursiveDescriptionModel ctx;
        auto schema = ctx.describeReflective(d);
        compare(ctx, ctx.bind(d), schema, row);
    }
    for (const auto &row : f.at("accessors")) {
        auto a = AccessorObject::fromAccessor("accessor", decodeAccessor(row.at("input").dump()));
        RecursiveDescriptionModel ctx;
        auto schema = ctx.describe(a);
        compare(ctx, ctx.bind(a), schema, row);
        Accessor replacement;
        replacement.name = "shared";
        auto shared = AccessorObject::fromAccessor("replacement", replacement);
        ctx.bind(shared);
        CheckedMemento editor(*ctx.bind(a), schema, &ctx.resolver());
        for (const auto &edit : row.at("edits"))
            editor.writeUsing(*ctx.resolver().descriptions.at(edit.at("field")), value(edit.at("value")));
        check(editor.validate().empty(), "GT accessor staged validation");
        editor.commit();
        check(J::parse(encodeAccessor(a->snapshot())) == row.at("after"), "GT accessor metadata mutation");
        check((a->accessor && a->accessor == a->next) == row.at("sharedSlots"),
              "GT shared accessor slot identity");
    }
    for (const auto &row : f.at("conditions")) {
        auto c = std::make_shared<ConditionObject>();
        c->id = "condition";
        c->predicate = decodePredicate(row.at("predicate").dump());
        RecursiveDescriptionModel ctx;
        auto schema = ctx.describe(c);
        compare(ctx, ctx.bind(c), schema, row);
        check(schema->children.empty(), "Faithful empty condition annotations");
        for (const auto &test : row.at("values"))
            check(c->evaluate(value(test.at("value"))) == test.at("result"),
                  "GT described condition evaluation");
    }
    auto d = prototype(f.at("descriptions").at(7));
    RecursiveDescriptionModel ctx;
    auto schema = ctx.describeReflective(d);
    compare(ctx, ctx.bind(d), schema, f.at("levels").at(0));
    auto a = d->accessorObject;
    auto keySchema = ctx.describe(a);
    compare(ctx, ctx.bind(a), keySchema, f.at("levels").at(1));
    auto alias = prototype(f.at("descriptions").at(7), "alias");
    alias->properties["label"] = Value("Alias");
    alias->accessor->name = "original";
    auto aliasSchema = ctx.describeReflective(alias);
    compare(ctx, ctx.bind(alias), aliasSchema, f.at("levels").at(2));
    DictionaryModel model;
    model.values = {{"Value", Value(std::int64_t(3))},
                    {"final", Value(std::int64_t(9))},
                    {"original", Value(std::int64_t(4))}};
    check(model.readUsing(*d) == value(f.at("finalReads").at("before")), "GT original access");
    CheckedMemento aliasEdit(*ctx.bind(alias), aliasSchema, &ctx.resolver());
    aliasEdit.writeUsing(*ctx.resolver().descriptions.at(aliasSchema->id + ".accessor"),
                         Value(Value::Reference{a->id}));
    check(aliasEdit.validate().empty(), "Shared accessor assignment validates");
    aliasEdit.commit();
    CheckedMemento accessorEdit(*ctx.bind(a), keySchema, &ctx.resolver());
    accessorEdit.writeUsing(*ctx.resolver().descriptions.at(keySchema->id + ".key"),
                            Value(Value::Symbol{"final"}));
    check(accessorEdit.validate().empty(), "Accessor key edit validates");
    accessorEdit.commit();
    check(d->accessorObject == alias->accessorObject &&
              model.readUsing(*d) == value(f.at("finalReads").at("after")) &&
              model.readUsing(*alias) == value(f.at("finalReads").at("alias")),
          "GT shared accessor editing changes actual model reads");
    auto schemas = ctx.schemas();
    check(canonical(J::parse(schemas.encode())) == canonical(f.at("finalSchemas")),
          "GT complete class-valued reflective schemas");
    auto appRoot = std::make_shared<Description>();
    appRoot->id = "app";
    appRoot->className = "MAContainer";
    auto classField = std::make_shared<Description>();
    classField->id = "klass";
    classField->className = "MAClassDescription";
    classField->key = "klass";
    appRoot->children = {d, classField};
    schemas.root = appRoot->id;
    schemas.descriptions.emplace(appRoot->id, appRoot);
    schemas.descriptions.emplace(d->id, d);
    schemas.descriptions.emplace(alias->id, alias);
    schemas.descriptions.emplace(classField->id, classField);
    AdaptiveModel object;
    object.id = "sample";
    object.description = appRoot;
    object.values = {{d->id, Value(std::int64_t(9))}, {"klass", Value(Value::Class{"String"})}};
    schemas.objects.emplace(object.id, object);
    rejects([&] { schemas.encode(); }, "Live accessor identity cannot be silently flattened");
    d->materializeAccessor();
    alias->materializeAccessor();
    auto wire = schemas.encode();
    auto imported = Graph::decode(wire);
    check(imported.encode() == wire, "Class-valued edited graph fixed point");
    check(validate(*imported.descriptions.at("app"), imported.objects.at("sample"), {}, &imported).empty(),
          "Imported class-valued graph validates");
    if (output) {
        std::ofstream out(output);
        out << wire;
        check(bool(out), "Native class graph export");
    }
}
void edges() {
    auto outer = std::make_shared<Description>();
    outer->id = "outer";
    outer->className = "MAContainer";
    auto nested = std::make_shared<Description>();
    nested->id = "nested";
    nested->className = "MAContainer";
    auto score = std::make_shared<Description>();
    score->id = "score";
    score->className = "MANumberDescription";
    score->key = "score";
    score->properties["required"] = Value(true);
    outer->children = {nested};
    nested->children = {score};
    Accessor childKey;
    childKey.name = "child";
    nested->accessorObject = AccessorObject::fromAccessor("nested.accessor", childKey);
    auto child = std::make_shared<DictionaryModel>();
    child->values["score"] = Value(std::int64_t(7));
    DictionaryModel parent;
    parent.values["child"] = Value(Value::Reference{"child"});
    Graph resolver;
    resolver.nativeObjects["child"] = child;
    check(validate(*outer, parent, {}, &resolver).empty(),
          "Nested container validation follows live accessor");
    check(writeString(*outer, parent, &resolver) == "7", "Nested container writer follows live accessor");
    Accessor identity;
    identity.className = "MAIdentityAccessor";
    auto portableRoot = std::make_shared<Description>();
    portableRoot->id = "root";
    portableRoot->className = "MAContainer";
    portableRoot->accessorObject = AccessorObject::fromAccessor("root.accessor", identity);
    Graph containerGraph;
    containerGraph.root = "root";
    containerGraph.descriptions["root"] = portableRoot;
    rejects([&] { containerGraph.encode(); }, "Live container accessor export is explicit");
    portableRoot->materializeAccessor();
    check(!containerGraph.encode().empty(), "Identity container materializes to implicit accessor");

    for (const auto &row : fixture("reflection-invalid")) {
        rejects(
            [&] {
                if (row.at("kind") == "value")
                    decodeValue(row.at("input").dump());
                else
                    decodeAccessor(row.at("input").dump());
            },
            "Shared reflection protocol rejection");
    }

    Accessor delegated;
    delegated.className = "MADelegatorAccessor";
    Accessor leaf;
    leaf.name = "alpha";
    delegated.steps = {leaf};
    DictionaryModel dict;
    dict.values["alpha"] = Value(std::int64_t(2));
    check(delegated.read(dict) == Value(std::int64_t(2)), "GT delegator forwards reads");
    delegated.write(dict, Value(std::int64_t(7)));
    check(dict.values.at("alpha") == Value(std::int64_t(7)), "GT delegator forwards writes");
    check(encodeAccessor(decodeAccessor(encodeAccessor(delegated))) == encodeAccessor(delegated),
          "Delegator codec fixed point");

    rejects([] { decodeValue(R"({"type":"class","value":"UnknownClass"})"); }, "Unknown classes rejected");
    check(Value(Value::Class{"String"}) != Value(Value::Symbol{"String"}), "Classes differ from Symbols");
    auto source = std::make_shared<Description>();
    source->id = "relation";
    source->className = "MAToOneRelationDescription";
    source->key = "relation";
    auto r = refFor(source);
    RecursiveDescriptionModel ctx;
    auto schema = ctx.describeReflective(source);
    auto model = ctx.bind(source);
    auto classes = ctx.resolver().descriptions.at(schema->id + ".classes");
    CheckedMemento editor(*model, schema, &ctx.resolver());
    editor.writeUsing(*classes, Value(Value::Collection{"set", {Value(Value::Class{"String"})}}));
    check(editor.validate().empty(), "Class-valued relation edit validates");
    editor.commit();
    check(source->classes == std::vector<std::string>{"String"}, "Classes setter uses real class values");
    check(validateValue(*source, Value("yes"), &ctx.resolver()).empty() &&
              !validateValue(*source, Value(std::int64_t(1)), &ctx.resolver()).empty(),
          "Edited class constraints govern values");
    rejects([&] { model->writeUsing(*classes, Value(Value::Array{Value("String")})); },
            "Text cannot impersonate Class");
    rejects(
        [&] {
            model->writeUsing(*ctx.resolver().descriptions.at(schema->id + ".stringReader"),
                              Value(Value::Class{"MAReader"}));
        },
        "Unavailable interpreter class rejected");
    auto chain = AccessorObject::fromAccessor(
        "chain",
        decodeAccessor(
            R"({"class":"MAChainAccessor","steps":[{"class":"MADictionaryAccessor","key":"child"},{"class":"MADictionaryAccessor","key":"leaf"}]})"));
    auto chainSchema = ctx.describe(chain);
    auto next = ctx.resolver().descriptions.at(chainSchema->id + ".next");
    auto previous = chain->next;
    rejects([&] { ctx.bind(chain)->writeUsing(*next, Value(Value::Reference{chain->id})); },
            "Accessor cycles rejected");
    check(chain->next == previous, "Failed cycle mutation restores slot");
    auto copied = copyDescription(source, "copy");
    check(copied.root->accessorObject != source->accessorObject,
          "Description copy clones live accessor identity");
    Limits tiny;
    tiny.nodes = 1;
    RecursiveDescriptionModel bounded(tiny);
    rejects([&] { bounded.bind(chain); }, "Accessor aggregate bounds");
    check(bounded.resolver().nativeObjects.empty(), "Rejected accessor bind is atomic");
    auto alien =
        AccessorObject::fromAccessor("relation", Accessor{"MADictionaryAccessor", "key", "", {}, {}});
    rejects([&] { ctx.bind(alien); }, "Cross-kind identity collision");
    Graph graph;
    graph.root = "root";
    auto root = std::make_shared<Description>();
    root->id = "root";
    root->className = "MAContainer";
    auto valueDesc = std::make_shared<Description>();
    valueDesc->id = "type";
    valueDesc->key = "type";
    root->children = {valueDesc};
    graph.descriptions = {{"root", root}, {"type", valueDesc}};
    AdaptiveModel o;
    o.id = "model";
    o.description = root;
    o.values = {{"type", Value("ordinary")}};
    graph.objects.emplace(o.id, o);
    check(!graph.encode().empty(), "Legacy fields named type remain valid");
    auto j = J::parse(graph.encode());
    j["descriptions"][1]["properties"]["metadata:class"] = J{{"type", "class"}, {"value", "String"}};
    rejects([&] { Graph::decode(j.dump()); }, "Legacy graph class values rejected");
    graph.format = "magritte.graph/4";
    Predicate p;
    p.selector = "=";
    p.arguments = {Value(Value::Class{"String"})};
    valueDesc->portableConditions = {{"class", p}};
    valueDesc->className = "MAStringDescription";
    o.values["type"] = Value(Value::Class{"String"});
    graph.objects["model"] = o;
    auto modern = J::parse(graph.encode());
    modern["format"] = "magritte.graph/3";
    rejects([&] { Graph::decode(modern.dump()); }, "Nested condition constants require graph/4");
    valueDesc->portableConditions.clear();
    valueDesc->accessor = delegated;
    graph.objects.clear();
    auto delegation = graph.encode();
    check(Graph::decode(delegation).encode() == delegation, "Delegator graph/4 fixed point");
    auto old = J::parse(delegation);
    old["format"] = "magritte.graph/3";
    rejects([&] { Graph::decode(old.dump()); }, "Legacy delegator rejected");
}
int main(int argc, char **argv) {
    try {
        reference(argc > 1 ? argv[1] : nullptr);
        edges();
        std::cout << checks << " reflection checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
