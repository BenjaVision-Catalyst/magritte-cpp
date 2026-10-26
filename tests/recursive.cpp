#include <algorithm>
#include <fstream>
#include <iostream>
#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool b, const std::string &message) {
    if (!b)
        throw Error(message);
    ++checks;
}
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, "Expected explicit rejection");
}
J fixture(const std::string &name) {
    std::ifstream in(std::string(FIXTURES) + "/" + name + ".json");
    J j;
    in >> j;
    return j;
}
J canonical(J j) {
    for (auto &node : j["descriptions"])
        if (node.contains("classes"))
            std::sort(node["classes"].begin(), node["classes"].end());
    return j;
}
std::shared_ptr<Description> prototype(const std::string &cls) {
    auto d = std::make_shared<Description>();
    d->id = "target";
    d->key = "Value";
    d->className = cls;
    Accessor binding;
    binding.name = "Value";
    d->accessor = binding;
    for (const auto &c : fixture("self-description-reference"))
        if (c.at("class") == cls) {
            for (auto p = c.at("properties").begin(); p != c.at("properties").end(); ++p)
                d->properties[p.key()] = decodeValue(p.value().dump());
            for (auto p = c.at("slots").begin(); p != c.at("slots").end(); ++p)
                d->properties[p.key()] = decodeValue(p.value().dump());
        }
    return d;
}
// Own the explicit default reference while constructing a context's retained bindings.
std::shared_ptr<Description> referenceFor(const std::shared_ptr<Description> &d) {
    auto r = std::make_shared<Description>();
    r->id = "target.reference";
    Accessor a;
    a.className = "MANullAccessor";
    r->accessor = a;
    if (d->isReference())
        d->reference = r;
    return r;
}
void compareRecord(RecursiveDescriptionModel &context, const std::shared_ptr<Description> &target,
                   const std::shared_ptr<Description> &schema, const J &record) {
    Graph level;
    level.root = schema->id;
    for (const auto &node : record.at("graph").at("descriptions")) {
        auto id = node.at("id").get<std::string>();
        level.descriptions.emplace(id, context.resolver().descriptions.at(id));
    }
    auto got = canonical(J::parse(level.encode()));
    check(got == canonical(record.at("graph")),
          "GT recursive schema " + target->className + "\n" + got.dump(2));
    auto binding = context.bind(target);
    for (const auto &read : record.at("reads")) {
        auto d = context.resolver().descriptions.at(read.at("field"));
        check(binding->readUsing(*d) == decodeValue(read.at("value").dump()),
              "GT recursive metadata getter " + target->className + " " + d->id + " got " +
                  encodeValue(binding->readUsing(*d)) + " expected " + read.at("value").dump());
    }
}
void composition() {
    auto reference = fixture("composition-reference");
    auto g = Graph::decode(reference.at("graph").dump());
    for (const auto &row : reference.at("equalities"))
        check(descriptionEqual(*g.descriptions.at(row.at("lhs")), *g.descriptions.at(row.at("rhs"))) ==
                  row.at("equal"),
              "GT equality matrix");
    int i = 0;
    for (const auto &row : reference.at("operations")) {
        auto a = g.descriptions.at(row.at("lhs"));
        auto b = row.at("rhs") == "" ? a : g.descriptions.at(row.at("rhs"));
        auto id = "composed." + std::to_string(++i);
        auto op = row.at("operation");
        DescriptionCopy result;
        if (op == "concat")
            result = concatenate(a, b, id);
        if (op == "union")
            result = unionDescriptions(a, b, id);
        if (op == "intersection")
            result = intersectDescriptions(a, b, id);
        if (op == "copy")
            result = copyDescription(a, id);
        if (op == "empty")
            result = copyEmpty(a, id);
        if (op == "range")
            result = copyRange(a, 1, 3, id);
        if (op == "select" || op == "reject")
            result = selectDescriptions(
                a,
                [&](const auto &d) {
                    return descriptionEqual(d, *g.descriptions.at("a")) == (op == "select");
                },
                id);
        if (op == "collect")
            result = collectDescriptions(
                a,
                [&](auto d) {
                    return descriptionEqual(*d, *g.descriptions.at("b")) ? g.descriptions.at("num") : d;
                },
                id);
        check(result.root->className == row.at("class") &&
                  result.root->property("label") == Value(row.at("label").get<std::string>()),
              "GT composition species and properties");
        auto children = result.root->orderedChildren();
        check(children.size() == row.at("children").size(), "GT composition multiplicity");
        if (op == "intersection") {
            for (const auto &d : children)
                check(descriptionEqual(*d, *g.descriptions.at(row.at("children").front())),
                      "GT intersection equality class");
            check(children.front() == g.descriptions.at("a"),
                  "Stable first native intersection representative");
        } else if (result.root->className == "MAPriorityContainer") {
            std::vector<std::string> got, expected = row.at("children");
            for (const auto &d : children)
                got.push_back(d->id);
            std::sort(got.begin(), got.end());
            std::sort(expected.begin(), expected.end());
            check(got == expected, "GT priority composition members; ties unspecified");
        } else {
            for (std::size_t n = 0; n < children.size(); ++n)
                check(children[n]->id == row.at("children").at(n), "GT composition child identity/order");
        }
    }
    auto option = g.descriptions.at("option"), num = g.descriptions.at("num"),
         left = g.descriptions.at("left");
    option->accessor = option->effectiveAccessor();
    auto copied = copyDescription(option, "copied");
    check(copied.root != option && copied.root->referenceDescription() != num &&
              descriptionEqual(*copied.root->referenceDescription(), *num),
          "Selective reference copy");
    copied.root->referenceDescription()->properties["min"] = Value(std::int64_t(9));
    check(num->property("min") == decodeValue(reference.at("copy").at("sourceMinAfterCopyEdit").dump()),
          "Independent copied reference properties");
    copied.root->accessor->name = "changed";
    check(option->effectiveAccessor().name == "option", "Independent copied accessor");
    copied.root->options.push_back(Value(std::int64_t(3)));
    check(option->options.size() == 2, "Independent option collection");
    auto container = copyDescription(left, "copied.container");
    container.root->properties["label"] = Value("Copied");
    check(left->property("label") == Value("Left"), "Independent container properties");
    g.descriptions.at("a")->properties["label"] = Value("Shared");
    check(container.root->children.front()->property("label") == Value("Shared"),
          "Container children remain shared");
    check(copyWithout(left, *g.descriptions.at("a"), "without").root->children ==
              std::vector<std::shared_ptr<Description>>{g.descriptions.at("b")},
          "copyWithout uses Magritte equality");
    check(copyWithoutAll(left, {g.descriptions.at("a"), g.descriptions.at("num")}, "withoutAll")
                  .root->children.size() == 1,
          "copyWithoutAll");
    check(asContainer(left, "ignored").root == left &&
              asContainer(num, "wrapped").root->children.front() == num,
          "asContainer identity/wrapping");
    rejects([&] { copyDescription(left, "left"); });
    rejects([&] { copyRange(left, 2, 10, "range"); });
    Limits tiny;
    tiny.visits = 1;
    rejects([&] { unionDescriptions(left, left, "bounded", tiny); });
    auto recursive = std::make_shared<Description>();
    recursive->id = "recursive";
    recursive->className = "MATokenDescription";
    recursive->reference = recursive;
    rejects([&] { copyDescription(recursive, "cycle.copy"); });
    auto independent = copyDescription(option, "lifetime");
    g = Graph{};
    option.reset();
    num.reset();
    left.reset();
    check(independent.root->referenceDescription()->property("min") == Value(std::int64_t(1)),
          "Copy result retains weak reference owners");
}
void recursive(const char *exportPath) {
    auto reference = fixture("recursive-reference");
    for (const auto &record : reference.at("classes")) {
        auto target = prototype(record.at("class"));
        auto ref = referenceFor(target);
        RecursiveDescriptionModel context;
        auto schema = context.describe(target);
        compareRecord(context, target, schema, record);
    }
    auto target = prototype("MANumberDescription");
    RecursiveDescriptionModel context;
    auto first = context.describe(target);
    compareRecord(context, target, first, reference.at("levels").at(0));
    auto second = context.describe(first);
    compareRecord(context, first, second, reference.at("levels").at(1));
    auto min = context.resolver().descriptions.at(first->id + ".min");
    auto third = context.describe(min);
    compareRecord(context, min, third, reference.at("levels").at(2));
    CheckedMemento metadata(*context.bind(min), third, &context.resolver());
    metadata.writeUsing(*context.resolver().descriptions.at(third->id + ".min"), Value(std::int64_t(7)));
    check(metadata.validate().empty(), "Metadata edit validates");
    metadata.commit();
    CheckedMemento editor(*context.bind(target), first, &context.resolver());
    check(editor.validate().at(0).code == reference.at("before").front(),
          "Recursive edit changes the next interpreter");
    editor.writeUsing(*min, Value(std::int64_t(8)));
    check(editor.validate().empty(), "Edited metadata accepts corrected value");
    editor.commit();
    CheckedMemento structure(*context.bind(first), second, &context.resolver());
    Value::Array remaining;
    for (const auto &d : first->children)
        if (d->effectiveAccessor().name != "comment" && d->effectiveAccessor().name != "max")
            remaining.push_back(Value(Value::Reference{d->id}));
    structure.writeUsing(*context.resolver().descriptions.at(second->id + ".children"),
                         Value(Value::Collection{"ordered", remaining}));
    check(structure.validate().empty(), "Self-described child collection validates");
    structure.commit();
    check(validate(*first, *context.bind(target), {}, &context.resolver()).empty(),
          "Edited recursive schema validates target");
    check(target->property("min") == decodeValue(reference.at("targetMin").dump()),
          "GT target metadata commit");
    auto schemas = context.schemas();
    check(canonical(J::parse(schemas.encode())) == canonical(reference.at("finalSchemas")),
          "GT three-level edited metadata graph");
    if (exportPath) {
        std::ofstream out(exportPath);
        out << schemas.encode();
    }
    auto imported = Graph::decode(schemas.encode());
    auto importedRoot = imported.descriptions.at(first->id);
    RecursiveDescriptionModel receiver;
    auto importedModel = receiver.bind(importedRoot);
    CheckedMemento importedEditor(*importedModel, imported.descriptions.at(second->id), &receiver.resolver());
    Value::Array importedChildren;
    for (std::size_t i = 0; i + 1 < importedRoot->children.size(); ++i)
        importedChildren.push_back(Value(Value::Reference{importedRoot->children[i]->id}));
    importedEditor.writeUsing(*imported.descriptions.at(second->id + ".children"),
                              Value(Value::Collection{"ordered", importedChildren}));
    check(importedEditor.validate().empty(), "Imported metadata remains executable without schema mapping");
    importedEditor.commit();
    check(importedRoot->children.size() == importedChildren.size(), "Imported structural metadata commit");
    rejects([&] { context.resolver().encode(); });
    auto children = context.resolver().descriptions.at(second->id + ".children");
    rejects([&] {
        context.bind(first)->writeUsing(
            *children, Value(Value::Collection{"ordered", {Value(Value::Reference{first->id})}}));
    });
    check(first->children.size() == remaining.size(), "Rejected owning cycle rolls back structure");
    Limits tiny;
    tiny.nodes = 1;
    RecursiveDescriptionModel bounded(tiny);
    rejects([&] { bounded.describe(prototype("MANumberDescription")); });
    check(bounded.resolver().nativeObjects.size() == 1,
          "Failed metadata build does not publish partial schema");
    std::shared_ptr<Model> orphan;
    {
        RecursiveDescriptionModel owner;
        orphan = owner.bind(prototype("MANumberDescription"));
    }
    rejects([&] { orphan->readUsing(*min); });
    auto other = prototype("MANumberDescription");
    rejects([&] { context.bind(other); });
    auto token = prototype("MATokenDescription");
    auto ref = referenceFor(token);
    RecursiveDescriptionModel relationships;
    auto tokenSchema = relationships.describe(token);
    auto newRef = std::make_shared<Description>();
    newRef->id = "replacement";
    newRef->className = "MANumberDescription";
    relationships.bind(newRef);
    CheckedMemento referenceEditor(*relationships.bind(token), tokenSchema, &relationships.resolver());
    referenceEditor.writeUsing(*relationships.resolver().descriptions.at(tokenSchema->id + ".reference"),
                               Value(Value::Reference{newRef->id}));
    check(referenceEditor.validate().empty(), "Self-described reference replacement validates");
    referenceEditor.commit();
    check(token->referenceDescription() == newRef, "Self-described reference setter preserves identity");
    auto structural = fixture("recursive-structural-reference");
    check(newRef->className == structural.at("referenceClass"), "GT reference edit type");
    auto option = std::make_shared<Description>();
    option->id = "options";
    option->className = "MASingleOptionDescription";
    option->reference = newRef;
    auto optionSchema = relationships.describe(option);
    auto field = relationships.resolver().descriptions.at(optionSchema->id + ".optionsTextual");
    auto binding = relationships.bind(option);
    for (const auto &row : structural.at("options")) {
        auto before = option->options;
        bool error = false;
        try {
            binding->writeUsing(*field, Value(row.at("text").get<std::string>()));
        } catch (const std::exception &) {
            error = true;
        }
        check(error == row.at("error"), "GT option-text failure");
        Value::Array expected;
        for (const auto &v : row.at("options"))
            expected.push_back(decodeValue(v.dump()));
        check(option->options == expected, "GT option-text values");
        check(binding->readUsing(*field) == Value(row.at("rendered").get<std::string>()),
              "GT option-text rendering");
        if (error)
            check(option->options == before, "Failed option parsing preserves prior options");
    }
}
int main(int argc, char **argv) {
    try {
        composition();
        recursive(argc > 1 ? argv[1] : nullptr);
        std::cout << checks << " composition/recursive checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
