// Oracle comparison: the same script, the same random sequence, N steps; ParticleUniverse (fa74677, Ogre 14.6) vs md::pu.
#include "ParticleUniversePCH.h"
#include "ParticleUniverseSystemManager.h"
#include "ParticleUniverseSystem.h"
#include "ParticleUniverseTechnique.h"
#include "ParticleEmitters/ParticleUniverseSphereSurfaceEmitterFactory.h"
#include "ParticleEmitters/ParticleUniverseBoxEmitterFactory.h"
#include "ParticleAffectors/ParticleUniverseColourAffectorFactory.h"
#include "ParticleAffectors/ParticleUniverseScaleAffectorFactory.h"
#include "ParticleAffectors/ParticleUniverseTextureRotatorFactory.h"
#include "ParticleAffectors/ParticleUniverseLinearForceAffectorFactory.h"
#include "ParticleRenderers/ParticleUniverseLightRendererFactory.h"
#include <OgreRoot.h>
#include <OgreControllerManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreLogManager.h>
#include "monkey_dust/particles/pu_system.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <algorithm>

// A renderer that draws nothing: the step does not depend on the renderer, and Ogre has no GPU-less billboard set.
struct NullRenderer : ParticleUniverse::ParticleRenderer {
    NullRenderer() { mRendererType = "Billboard"; }
    void _prepare(ParticleUniverse::ParticleTechnique*) override {}
    void _setMaterialName(const ParticleUniverse::String&) override {}
    void _notifyCurrentCamera(ParticleUniverse::Camera*) override {}
    void _notifyAttached(Ogre::Node*, bool = false) override {}
    void _notifyParticleQuota(size_t) override {}
    void _notifyDefaultDimensions(ParticleUniverse::Real, ParticleUniverse::Real, ParticleUniverse::Real) override {}
    void _notifyParticleResized(void) override {}
    ParticleUniverse::SortMode _getSortMode(void) const override { return ParticleUniverse::SM_DISTANCE; }
    void copyAttributesTo(ParticleUniverse::ParticleRenderer*) override {}
};
struct FakeBillboard : ParticleUniverse::LightRendererFactory {
    ParticleUniverse::String getRendererType(void) const override { return "Billboard"; }
    ParticleUniverse::ParticleRenderer* createRenderer(void) override { return new NullRenderer(); }
};

struct Prov : Ogre::Math::RandomValueProvider {
    uint32_t s;
    float getRandomUnit() override { s = s * 214013u + 2531011u; return float((s >> 16) & 0x7fff) / 32767.0f; }
};

struct Row { float v[19]; };   // orig(3) pos(3) dir(3) col(4) w h d zrot ttl total

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: oracle script.pu steps dt seed [technique-name]\n"); return 2; }
    std::ifstream in(argv[1]); std::stringstream ss; ss << in.rdbuf(); std::string text = ss.str();
    int steps = std::atoi(argv[2]); float dt = std::strtof(argv[3], nullptr); uint32_t seed = (uint32_t)std::strtoul(argv[4], nullptr, 10);
    // one technique only (the LOD techniques of the original are switched by camera distance, which this run has none of)
    size_t first = text.find("\n    technique "), cut = text.find("\n    technique ", first + 1);
    std::string only = cut == std::string::npos ? text : text.substr(0, cut) + "\n}\n";
    // the Light renderer needs no GPU
    std::string oracle_text = only; size_t r = oracle_text.find("renderer");
    size_t b1 = oracle_text.find('{', r), b2 = oracle_text.find('}', b1);
    oracle_text = oracle_text.substr(0, r) + "renderer Billboard\n" + oracle_text.substr(b2 + 1);
    { std::ofstream o("/tmp/pu_oracle_scripts/one.pu"); o << oracle_text; }

    Ogre::LogManager* lm = new Ogre::LogManager(); lm->createLog("null.log", true, false, true);
    Ogre::Root* root = new Ogre::Root("", "", "");
    auto* sm = root->createSceneManager();
    if (!Ogre::ControllerManager::getSingletonPtr()) new Ogre::ControllerManager();
    auto* psm = new ParticleUniverse::ParticleSystemManager();
    psm->addEmitterFactory(new ParticleUniverse::SphereSurfaceEmitterFactory());
    psm->addEmitterFactory(new ParticleUniverse::BoxEmitterFactory());
    psm->addAffectorFactory(new ParticleUniverse::ColourAffectorFactory());
    psm->addAffectorFactory(new ParticleUniverse::ScaleAffectorFactory());
    psm->addAffectorFactory(new ParticleUniverse::TextureRotatorFactory());
    psm->addAffectorFactory(new ParticleUniverse::LinearForceAffectorFactory());
    psm->addRendererFactory(new FakeBillboard());
    Ogre::ResourceGroupManager::getSingleton().addResourceLocation("/tmp/pu_oracle_scripts", "FileSystem");
    Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
    std::vector<ParticleUniverse::String> names; psm->particleSystemTemplateNames(names);
    if (names.empty()) { std::fprintf(stderr, "oracle: no template parsed\n"); return 3; }
    auto* ps = psm->createParticleSystem("t", names[0], sm);
    if (!ps) { std::fprintf(stderr, "oracle: no system\n"); return 3; }
    auto* node = sm->getRootSceneNode()->createChildSceneNode(); node->attachObject(ps);

    Prov prov; prov.s = seed; Ogre::Math::SetRandomValueProvider(&prov);
    ps->start();
    for (int i = 0; i < steps; ++i) ps->_update(dt);
    std::vector<Row> orc;
    ParticleUniverse::ParticleTechnique* tech = ps->getTechnique(0);
    auto& list = tech->_getParticlePool()->getVisualParticlesList();
    for (auto* p : list) {
        if (p->hasEventFlags(ParticleUniverse::Particle::PEF_EXPIRED)) continue;   // expired in the last step: locked, not alive
        Row w{}; float* v = w.v;
        v[0]=p->originalPosition.x; v[1]=p->originalPosition.y; v[2]=p->originalPosition.z;
        v[3]=p->position.x; v[4]=p->position.y; v[5]=p->position.z;
        v[6]=p->direction.x; v[7]=p->direction.y; v[8]=p->direction.z;
        v[9]=p->colour.r; v[10]=p->colour.g; v[11]=p->colour.b; v[12]=p->colour.a;
        v[13]=p->width; v[14]=p->height; v[15]=p->depth; v[16]=p->zRotation.valueRadians(); v[17]=p->timeToLive; v[18]=p->totalTimeToLive;
        orc.push_back(w);
    }

    // ours
    std::vector<md::pu::Node> nodes; std::string err;
    if (!md::pu::Parse(only, nodes, err)) { std::fprintf(stderr, "ours: parse %s\n", err.c_str()); return 4; }
    static md::pu::SystemDef def;
    if (!md::pu::LoadSystem(nodes[0], def, err)) { std::fprintf(stderr, "ours: load %s\n", err.c_str()); return 4; }
    uint32_t st = seed; static md::pu::Instance inst; inst.Init(def, 0, md::pu::MsvcRng(&st));
    for (int i = 0; i < steps; ++i) inst.Update(dt);
    std::vector<Row> our;
    for (int i = 0; i < inst.Count(); ++i) {
        const auto& p = inst.Get(i); Row w{}; float* v = w.v;
        v[0]=p.origin.x; v[1]=p.origin.y; v[2]=p.origin.z; v[3]=p.pos.x; v[4]=p.pos.y; v[5]=p.pos.z; v[6]=p.dir.x; v[7]=p.dir.y; v[8]=p.dir.z;
        v[9]=p.colour.r; v[10]=p.colour.g; v[11]=p.colour.b; v[12]=p.colour.a; v[13]=p.w; v[14]=p.h; v[15]=p.d; v[16]=p.zrot; v[17]=p.ttl; v[18]=p.total_ttl;
        our.push_back(w);
    }
    auto less = [](const Row& a, const Row& b) { for (int i = 0; i < 3; ++i) if (a.v[i] != b.v[i]) return a.v[i] < b.v[i]; return false; };
    std::sort(orc.begin(), orc.end(), less); std::sort(our.begin(), our.end(), less);
    std::printf("oracle particles %zu, ours %zu (steps %d, dt %g, seed %u)\n", orc.size(), our.size(), steps, dt, seed);
    if (orc.size() != our.size()) {
        for (auto& a : orc) { bool f = false; for (auto& b : our) if (a.v[0]==b.v[0]&&a.v[1]==b.v[1]&&a.v[2]==b.v[2]) f = true; if (!f) std::printf("only oracle: ttl %.6f total %.6f dt %g ttl/dt %.4f\n", a.v[17], a.v[18], dt, a.v[17]/dt); }
        for (auto& a : our) { bool f = false; for (auto& b : orc) if (a.v[0]==b.v[0]&&a.v[1]==b.v[1]&&a.v[2]==b.v[2]) f = true; if (!f) std::printf("only ours: ttl %.6f total %.6f\n", a.v[17], a.v[18]); }
        for (auto& a : orc) { bool f = false; for (auto& b : our) if (a.v[0]==b.v[0]&&a.v[1]==b.v[1]&&a.v[2]==b.v[2]) f = true; if (f) continue;
            uint32_t st2 = seed; static md::pu::Instance i2; i2.Init(def, 0, md::pu::MsvcRng(&st2)); bool seen = false; float lastttl = 0;
            for (int s = 1; s <= steps; ++s) { i2.Update(dt); bool here = false; for (int k = 0; k < i2.Count(); ++k) { auto& q = i2.Get(k); if (q.origin.x==a.v[0]&&q.origin.y==a.v[1]&&q.origin.z==a.v[2]) { here = true; lastttl = q.ttl; } }
                if (here && !seen) { seen = true; std::printf("ours: born at step %d\n", s); } if (!here && seen) { std::printf("ours: gone at step %d, ttl before %.6f (dt %g)\n", s, lastttl, dt); break; } } }
        return 1; }
    if (std::getenv("DUMP")) for (size_t i = 0; i < 3 && i < orc.size(); ++i) { std::printf("O"); for (int k = 0; k < 19; ++k) std::printf(" %.5g", orc[i].v[k]); std::printf("\nU"); for (int k = 0; k < 19; ++k) std::printf(" %.5g", our[i].v[k]); std::printf("\n"); }
    const char* nm[19] = {"ox","oy","oz","px","py","pz","dx","dy","dz","r","g","b","a","w","h","d","zrot","ttl","total"};
    double worst[19] = {}; 
    for (size_t i = 0; i < orc.size(); ++i) for (int k = 0; k < 19; ++k) {
        double a = orc[i].v[k], b = our[i].v[k], e = std::fabs(a - b) / std::max(1.0, std::fabs(a)); if (e > worst[k]) worst[k] = e; }
    double all = 0; for (int k = 0; k < 19; ++k) { std::printf("%s %.3g  ", nm[k], worst[k]); all = std::max(all, worst[k]); }
    std::printf("\nMAX relative-to-max(1,|x|) difference %.3g\n", all);
    return all < 1e-3 ? 0 : 1;
}
