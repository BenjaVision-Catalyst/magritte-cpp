#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>
using namespace magritte;
using J = nlohmann::json;
int checks = 0;
void check(bool ok, const std::string& message) { if (!ok) throw Error(message); ++checks; }
template<class F> void rejects(F f) { bool failed=false; try { f(); } catch(const Error&) { failed=true; } check(failed,"Expected rejection"); }
J fixture() { std::ifstream in(std::string(FIXTURES)+"/visitor-reference.json"); J j; in>>j; return j; }
std::string label(const Description& d) { return std::get<std::string>(d.property("label").data); }
struct Probe : Visitor {
    using Visitor::Visitor;
    J events = J::array();
    std::string stopAt, failAt;
    bool recurse = false;
    bool record(const std::string& name, Description& d) {
        events.push_back(J::array({name,label(d)}));
        if (name==failAt) throw Error("visitor failure");
        return name!=stopAt;
    }
    void visitDescription(Description& d) override { if (record("visitDescription:",d)) { Visitor::visitDescription(d); } }
    void visitContainer(Description& d) override { if (record("visitContainer:",d)) { Visitor::visitContainer(d); if (recurse) visitAll(d.orderedChildren()); } }
    void visitPriorityContainer(Description& d) override { if (record("visitPriorityContainer:",d)) { Visitor::visitPriorityContainer(d); } }
    void visitElementDescription(Description& d) override { if (record("visitElementDescription:",d)) { Visitor::visitElementDescription(d); } }
    void visitBooleanDescription(Description& d) override { if (record("visitBooleanDescription:",d)) { Visitor::visitBooleanDescription(d); } }
    void visitClassDescription(Description& d) override { if (record("visitClassDescription:",d)) { Visitor::visitClassDescription(d); } }
    void visitStringDescription(Description& d) override { if (record("visitStringDescription:",d)) { Visitor::visitStringDescription(d); } }
    void visitMemoDescription(Description& d) override { if (record("visitMemoDescription:",d)) { Visitor::visitMemoDescription(d); } }
    void visitPasswordDescription(Description& d) override { if (record("visitPasswordDescription:",d)) { Visitor::visitPasswordDescription(d); } }
    void visitSymbolDescription(Description& d) override { if (record("visitSymbolDescription:",d)) { Visitor::visitSymbolDescription(d); } }
    void visitMagnitudeDescription(Description& d) override { if (record("visitMagnitudeDescription:",d)) { Visitor::visitMagnitudeDescription(d); } }
    void visitNumberDescription(Description& d) override { if (record("visitNumberDescription:",d)) { Visitor::visitNumberDescription(d); } }
    void visitReferenceDescription(Description& d) override { if (record("visitReferenceDescription:",d)) { Visitor::visitReferenceDescription(d); } }
    void visitTokenDescription(Description& d) override { if (record("visitTokenDescription:",d)) { Visitor::visitTokenDescription(d); } }
    void visitOptionDescription(Description& d) override { if (record("visitOptionDescription:",d)) { Visitor::visitOptionDescription(d); } }
    void visitSingleOptionDescription(Description& d) override { if (record("visitSingleOptionDescription:",d)) { Visitor::visitSingleOptionDescription(d); } }
    void visitMultipleOptionDescription(Description& d) override { if (record("visitMultipleOptionDescription:",d)) { Visitor::visitMultipleOptionDescription(d); } }
    void visitRelationDescription(Description& d) override { if (record("visitRelationDescription:",d)) { Visitor::visitRelationDescription(d); } }
    void visitToOneRelationDescription(Description& d) override { if (record("visitToOneRelationDescription:",d)) { Visitor::visitToOneRelationDescription(d); } }
    void visitToManyRelationDescription(Description& d) override { if (record("visitToManyRelationDescription:",d)) { Visitor::visitToManyRelationDescription(d); } }
    void visitToManyScalarRelationDescription(Description& d) override { if (record("visitToManyScalarRelationDescription:",d)) { Visitor::visitToManyScalarRelationDescription(d); } }
};
struct Builder : DescriptionBuilder {
    using DescriptionBuilder::DescriptionBuilder;
    J events = J::array();
    bool fail = false;
    void visitStringDescription(Description& d) override {
        events.push_back(label(d));
        if (fail) throw Error("builder failure");
        d.properties["label"] = Value("Built "+label(d));
        DescriptionBuilder::visitStringDescription(d);
    }
};
struct Tree { std::shared_ptr<Description> root; std::vector<std::shared_ptr<Description>> retained; };
Tree tree(const J& j, const std::string& id) {
    Tree t; auto d = std::make_shared<Description>(); t.root=d; t.retained.push_back(d);
    d->id=id; d->className=j.at("class"); d->key=j.at("key");
    d->properties["label"]=Value(j.at("label").get<std::string>());
    d->properties["priority"]=Value(j.at("priority").get<std::int64_t>());
    for (const auto& row : j.at("children")) {
        auto c=tree(row,id+".child."+std::to_string(d->children.size())); d->children.push_back(c.root);
        t.retained.insert(t.retained.end(),c.retained.begin(),c.retained.end());
    }
    if (d->isReference()) {
        auto r=std::make_shared<Description>(); r->id=id+".reference"; r->properties["label"]=Value("Reference");
        Accessor null; null.className="MANullAccessor"; r->accessor=null;
        d->reference=r; t.retained.push_back(r);
        if(d->isOption()) d->options={Value("a"),Value("b")};
    }
    return t;
}
J snapshot(const std::shared_ptr<Description>& d) {
    J children=J::array();
    if(d->isContainer()) for(const auto& c:d->orderedChildren()) children.push_back(snapshot(c));
    return J{{"class",d->className},{"key",d->effectiveAccessor().name},{"label",label(*d)},{"priority",d->priority()},{"children",children}};
}
int main(int argc,char** argv) { try {
    auto f=fixture();
    for(const auto& row:f.at("dispatch")) {
        Description d; d.className=row.at("class"); d.properties["label"]=Value(d.className);
        Probe p; p.visit(d); check(p.events==row.at("events"),"Dispatch "+d.className);
        p.events=J::array(); d.acceptMagritte(p); check(p.events==row.at("events"),"Direct accept "+d.className);
    }
    auto input=tree(f.at("builders").at(0).at("source"),"source");
    input.root->children[2]=input.root->children[0];
    auto a=input.root->children[0], b=input.root->children[1];
    auto v=f.at("visiting");
    Probe p; p.visit(*input.root); check(p.events==v.at("plainContainer"),"No implicit traversal");
    p.events=J::array(); p.recurse=true; p.visit(*input.root); check(p.events==v.at("recursiveContainer"),"Explicit recursion preserves duplicates");
    p.events=J::array(); p.recurse=false; p.stopAt="visitStringDescription:"; p.visitAll({a,b,a}); check(p.events==v.at("stoppedBatch"),"Override can stop fallback");
    a->properties["visible"]=Value(false); a->properties["readOnly"]=Value(true);
    p.events=J::array(); p.stopAt=""; p.visit(*a); check(p.events==v.at("hiddenReadOnly"),"Visitor does not filter");
    p.events=J::array(); p.failAt="visitStringDescription:"; rejects([&]{p.visitAll({a,b});}); check(p.events==v.at("failedBatch"),"Failure aborts batch");
    p.failAt=""; p.events=J::array(); p.visit(*b); check(p.events.size()==4,"Exception restores visitor state");
    DictionaryModel target; target.values={{"first",Value("Ada")},{"second",Value(std::int64_t(7))},{"choice",Value("a")}};
    Graph output; output.format="magritte.graph/4";
    for(const auto& row:f.at("builders")) {
        auto src=tree(row.at("source"),"source."+row.at("case").get<std::string>());
        Builder builder; auto result=builder.buildFrom(src.root,"built."+row.at("case").get<std::string>(),&target);
        check(snapshot(result.root)==row.at("result"),"Builder result "+row.at("case").get<std::string>());
        check(snapshot(src.root)==row.at("sourceAfter"),"Builder source"); check(builder.events==row.at("events"),"Builder visits");
        check((result.root==src.root)==row.at("sameRoot").get<bool>(),"Root identity");
        check(builder.target==&target && builder.model==nullptr && builder.priority==0,"Builder context");
        if(row.at("case")=="flat") {
            output.root=result.root->id;
            check(validate(*result.root,target).empty(),"Built descriptions drive normal native validation");
            target.writeUsing(*result.root->children.front(),Value("Augusta"));
            check(target.readUsing(*result.root->children[2])==Value("Augusta"),"Copied accessors bind the same host field");
            target.values["first"]=Value("Ada");
            AdaptiveModel sample; sample.id="sample"; sample.description=result.root;
            for(const auto& d:result.root->children) sample.writeUsing(*d,target.readUsing(*d));
            check(validate(*result.root,sample).empty(),"Built adaptive schema validates");
            std::ifstream roundtrip(std::string(FIXTURES)+"/visitor-roundtrip-reference.json"); J expected; roundtrip>>expected;
            check(writeString(*result.root,sample)==expected[0]["reads"][0]["text"],"GT writer consumes built schema");
            output.objects.emplace(sample.id,std::move(sample));
            visit(*result.root,[&](const auto& d){for(const auto& owner:result.retained)if(owner.get()==&d)output.descriptions.emplace(d.id,owner);});
            for(const auto& d:result.root->children) check(std::find(src.root->children.begin(),src.root->children.end(),d)==src.root->children.end(),"Fresh child copies");
            check(result.root->children[0]!=result.root->children[2],"Duplicate occurrences copied separately");
            auto original=src.root->children.back(), copied=result.root->children.back();
            check(original->referenceDescription()!=copied->referenceDescription(),"Copied reference");
            copied->options.push_back(Value("c")); check(original->options.size()==2,"Copied option snapshot"); copied->options.pop_back();
            builder.model=&target; builder.priority=9;
            auto reused=builder.buildFrom(b,"reused"); check(reused.root==result.root && reused.root->children.size()==f.at("identity").at("reuseContainerCount"),"Builder retains accumulated result");
            check(builder.model==&target && builder.priority==9,"Builder explicit context");
            // Keep the exported graph at its independently checked pre-reuse contents.
            reused.root->children.pop_back();
        }
    }
    DescriptionBuilder scalar;
    auto one=scalar.buildFrom(a,"one"); check(one.root==a,"Base scalar keeps identity");
    rejects([&]{scalar.buildFrom(b,"two");}); scalar.setBuiltDescription(nullptr);
    check(scalar.buildFrom(b,"three").root==b,"Explicit reset");
    rejects([&]{scalar.buildFrom(a,"bad prefix");});
    Builder fails; fails.fail=true; rejects([&]{fails.buildFrom(input.root,"fail");});
    check(label(*a)=="First","Failed container build leaves source unchanged");
    fails.fail=false; check(fails.buildFrom(input.root,"recovered").root->children.size()==4,"Builder reusable after failure");
    struct CustomDescription : Description { void acceptMagritte(Visitor& visitor) override { visitor.visitStringDescription(*this); } } custom;
    custom.className="MANumberDescription"; custom.properties["label"]=Value("Custom");
    Probe customProbe; customProbe.visit(custom); check(customProbe.events[0][0]=="visitStringDescription:","Native description overrides acceptMagritte");
    Description unknown; unknown.className="MissingDescription"; rejects([&]{p.visit(unknown);});
    unknown.className="Object"; rejects([&]{p.visit(unknown);});
    ClassRegistry::global().registerClass({"VisitorInheritedMemo","MAMemoDescription",false,"Inherited"});
    unknown.className="VisitorInheritedMemo"; unknown.properties["label"]=Value("Inherited");
    p.events=J::array(); p.visit(unknown); check(p.events.size()==4 && p.events[0][0]=="visitMemoDescription:","Registered class inherits accept protocol");
    auto info=ClassRegistry::global().find("MADescriptionBuilder"); check(info && info->superclass==f.at("builderClass").at("superclass") && info->label==f.at("builderClass").at("label") && info->isAbstract==f.at("builderClass").at("abstract"),"GT builder class discovery");
    auto classValue=Value(Value::Class{"MADescriptionBuilder"});
    check(decodeValue(encodeValue(classValue))==classValue,"Builder Class value round trip");
    auto relation=std::make_shared<Description>(); relation->id="visitor.relation"; relation->className="MAToOneRelationDescription";
    auto reference=std::make_shared<Description>(); reference->id="visitor.reference"; reference->className="MAContainer"; relation->reference=reference;
    RecursiveDescriptionModel metadata; auto schema=metadata.describeReflective(relation);
    const auto& choices=metadata.resolver().descriptions.at(schema->id+".classes")->options;
    check(std::find(choices.begin(),choices.end(),classValue)!=choices.end(),"New metadata discovers builder class without a projection");
    Limits small; small.visits=2; Probe limited(small); rejects([&]{limited.visitAll({a,b});});
    limited.visit(*a);
    small=Limits{}; small.items=1; Probe items(small); rejects([&]{items.visitAll({a,b});});
    rejects([&]{p.visitAll({a,nullptr});});
    small=Limits{}; small.depth=2; Probe cycles(small); cycles.recurse=true;
    auto cycle=std::make_shared<Description>(); cycle->className="MAContainer"; cycle->id="cycle"; cycle->properties["label"]=Value("Cycle"); cycle->children={cycle};
    rejects([&]{cycles.visit(*cycle);}); cycle->children.clear(); cycles.visit(*cycle);
    small=Limits{}; small.nodes=2; DescriptionBuilder nodes(small); rejects([&]{nodes.buildFrom(input.root,"nodes");});
    small=Limits{}; small.items=1; DescriptionBuilder children(small); rejects([&]{children.buildFrom(input.root,"items");});
    auto clash=tree(f.at("builders").at(0).at("source"),"clash.1"); DescriptionBuilder collision; rejects([&]{collision.buildFrom(clash.root,"clash");});
    struct Skip:DescriptionBuilder {void visitDescription(Description&) override {}} skip;
    check(!skip.buildFrom(a,"skip").root,"Suppressed scalar returns no built description");
    if(argc>1) { std::ofstream out(argv[1]); out<<output.encode(); }
    std::cout<<checks<<" visitor checks passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;} }
