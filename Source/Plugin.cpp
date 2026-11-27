#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace {
struct Spec { const char* id; const char* name; float lo, hi, step, initial; const char* unit; };
constexpr std::array<Spec, 15> specs {{
    {"input","INPUT",-18,18,0.1f,0," dB"},
    {"gate","GATE",-80,-20,0.1f,-65," dB"},
    {"low","LOW CUT",45,240,1,80," Hz"},
    {"body","BODY",-9,9,0.1f,0," dB"},
    {"air","AIR",-9,9,0.1f,1," dB"},
    {"comp","COMPRESS",-40,0,0.1f,-18," dB"},
    {"ratio","RATIO",1,8,0.1f,3," :1"},
    {"deess","DE-ESS",0,100,1,25," %"},
    {"drive","DRIVE",0,100,1,10," %"},
    {"width","DOUBLE",0,100,1,0," %"},
    {"delay","DELAY",0,100,1,8," %"},
    {"time","TIME",60,750,1,250," ms"},
    {"verb","SPACE",0,100,1,8," %"},
    {"output","OUTPUT",-18,12,0.1f,0," dB"},
    {"limit","CEILING",-12,0,0.1f,-1," dB"}
}};
juce::AudioProcessorValueTreeState::ParameterLayout layout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    for (auto& s : specs)
        p.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{s.id,1}, s.name,
            juce::NormalisableRange<float>{s.lo,s.hi,s.step},s.initial,
            juce::AudioParameterFloatAttributes().withLabel(s.unit)));
    p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"bypass",1},"Bypass",false));
    return p;
}
float gain(float db) { return juce::Decibels::decibelsToGain(db); }
class Engine final : public juce::AudioProcessor
{
public:
    Engine() : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
        .withOutput("Output",juce::AudioChannelSet::stereo(),true)), state(*this,nullptr,"JAM3S",layout()) {}
    juce::AudioProcessorValueTreeState state;
    std::atomic<float> peak {0}, inputPeak {0};
    const juce::String getName() const override { return "JAM3S Vocal Engine"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int,const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool isBusesLayoutSupported(const BusesLayout& b) const override
    {
        auto o=b.getMainOutputChannelSet();
        return (o==juce::AudioChannelSet::mono() || o==juce::AudioChannelSet::stereo())
            && o==b.getMainInputChannelSet();
    }
    void prepareToPlay(double sr,int) override
    {
        rate=sr; history.setSize(2,juce::jmax(2,int(sr*2)+2)); history.clear(); pos=0;
        hp.fill(0); low.fill(0); high.fill(0); env.fill(0); gateEnv.fill(1);
        rev.setSampleRate(sr); rev.reset(); peak.store(0);
    }
    void releaseResources() override {}
    float value(const char* id) const { return state.getRawParameterValue(id)->load(); }
    void processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        inputPeak.store(b.getNumChannels()>0 && b.getNumSamples()>0 ? b.getMagnitude(0,b.getNumSamples()):0.f);
        if (value("bypass")>0.5f || history.getNumSamples()<2) { peak.store(b.getNumChannels()>0 && b.getNumSamples()>0 ? b.getMagnitude(0,b.getNumSamples()):0.f); return; }
        const int n=b.getNumSamples(), nc=juce::jmin(2,b.getNumChannels()), size=history.getNumSamples();
        if(nc==0 || n==0) return;
        auto coeff=[&](float f){return 1.0f-std::exp(-6.28318530718f*f/float(rate));};
        const float aHP=coeff(value("low")), aL=coeff(400), aH=coeff(5200);
        const float attack=coeff(25), release=coeff(2), gateA=coeff(15);
        const float input=gain(value("input")), out=gain(value("output")), threshold=gain(value("comp"));
        const float body=gain(value("body"))-1, air=gain(value("air"))-1, ratio=value("ratio");
        const float gate=gain(value("gate")), de=value("deess")*.01f, drive=value("drive")*.01f;
        const float wet=value("delay")*.0035f, width=value("width")*.003f;
        const float ceiling=gain(value("limit"));
        const int d=juce::jlimit(1,size-1,int(value("time")*.001*rate));
        const int shortDelay=juce::jlimit(1,size-1,int(.018*rate));
        for(int i=0;i<n;++i) {
            const int tap=(pos+size-d)%size, dbl=(pos+size-shortDelay)%size;
            for(int c=0;c<nc;++c) {
                float x=b.getSample(c,i)*input;
                hp[c]+=aHP*(x-hp[c]); x-=hp[c];
                gateEnv[c]+=gateA*((std::abs(x)>gate?1.0f:0.08f)-gateEnv[c]); x*=gateEnv[c];
                low[c]+=aL*(x-low[c]); high[c]+=aH*(x-high[c]);
                const float h=x-high[c];
                x+=body*low[c]+air*h;
                x-=h*de*juce::jlimit(0.0f,0.85f,std::abs(h)*5.0f);
                const float level=std::abs(x);
                env[c]+=(level>env[c]?attack:release)*(level-env[c]);
                if(env[c]>threshold) x*=std::pow(threshold/juce::jmax(env[c],1e-7f),1.0f-1.0f/ratio);
                if(drive>0.001f) {float k=1+drive*4; x=x*(1-drive)+drive*std::tanh(x*k)/k;}
                const float echo=history.getSample(c,tap);
                const float doubling=history.getSample(nc==2?1-c:c,dbl);
                history.setSample(c,pos,x+echo*.27f);
                b.setSample(c,i,x+echo*wet+doubling*width);
            }
            pos=(pos+1)%size;
        }
        juce::Reverb::Parameters rp;
        rp.roomSize=.38f; rp.damping=.55f; rp.width=1.0f;
        rp.wetLevel=value("verb")*.003f; rp.dryLevel=1.0f; rp.freezeMode=0;
        rev.setParameters(rp);
        if(nc==2) rev.processStereo(b.getWritePointer(0),b.getWritePointer(1),n);
        else rev.processMono(b.getWritePointer(0),n);
        float mx=0;
        for(int c=0;c<nc;++c) for(int i=0;i<n;++i) {
            float x=juce::jlimit(-ceiling,ceiling,b.getSample(c,i)*out);
            b.setSample(c,i,x); mx=juce::jmax(mx,std::abs(x));
        }
        peak.store(mx);
    }
    void getStateInformation(juce::MemoryBlock& dest) override
    { if(auto xml=state.copyState().createXml()) copyXmlToBinary(*xml,dest); }
    void setStateInformation(const void* data,int bytes) override
    { if(auto xml=getXmlFromBinary(data,bytes)) if(xml->hasTagName(state.state.getType())) state.replaceState(juce::ValueTree::fromXml(*xml)); }
private:
    double rate=44100; int pos=0;
    std::array<float,2> hp{},low{},high{},env{},gateEnv{};
    juce::AudioBuffer<float> history;
    juce::Reverb rev;
};

class ChromeLook final : public juce::LookAndFeel_V4
{
public:
    ChromeLook() {
        setColour(juce::Slider::textBoxTextColourId,juce::Colour(0xffeeeeee));
        setColour(juce::Slider::textBoxOutlineColourId,juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxBackgroundColourId,juce::Colour(0xff15151b));
        setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff24242a));
        setColour(juce::ComboBox::textColourId,juce::Colours::white);
        setColour(juce::ToggleButton::textColourId,juce::Colours::white);
    }
    void drawRotarySlider(juce::Graphics& g,int x,int y,int w,int h,float p,float start,float end,juce::Slider&) override {
        const auto neon=juce::Colour(0xffff163e);
        auto box=juce::Rectangle<float>(float(x),float(y),float(w),float(h)).reduced(12);
        float r=juce::jmin(box.getWidth(),box.getHeight())*.5f;
        auto c=box.getCentre();
        auto ring=juce::Rectangle<float>(2*r,2*r).withCentre(c);
        for(int i=8;i>0;--i){
            g.setColour(juce::Colours::black.withAlpha(.09f));
            g.fillEllipse(ring.expanded(float(i)*.6f).translated(0,5));
        }
        juce::ColourGradient halo(neon.withAlpha(.38f),c.x,c.y,neon.withAlpha(0.f),c.x+r+12,c.y,true);
        g.setGradientFill(halo);g.fillEllipse(ring.expanded(12));
        g.setColour(juce::Colour(0xff030406));g.fillEllipse(ring.expanded(3));
        juce::ColourGradient rim(juce::Colour(0xfffafcff),c.x,c.y-r,juce::Colour(0xff7a828e),c.x,c.y+r,false);
        rim.addColour(.16,juce::Colour(0xff737b89));rim.addColour(.32,juce::Colour(0xffe7edf5));
        rim.addColour(.48,juce::Colour(0xff11151b));rim.addColour(.53,juce::Colour(0xff4a515c));rim.addColour(.77,juce::Colour(0xffb8c0cb));
        g.setGradientFill(rim);g.fillEllipse(ring);
        auto inset=ring.reduced(r*.15f);
        g.setColour(juce::Colour(0xff07090c));g.fillEllipse(inset);
        auto cap=ring.reduced(r*.25f);
        juce::ColourGradient face(juce::Colour(0xffd7dde6),c.x-r*.6f,c.y-r*.7f,juce::Colour(0xff11151c),c.x+r*.7f,c.y+r*.7f,false);
        face.addColour(.23,juce::Colour(0xff7a8391));face.addColour(.48,juce::Colour(0xff20252e));face.addColour(.52,juce::Colour(0xffaeb6c2));face.addColour(.77,juce::Colour(0xff363d47));
        g.setGradientFill(face);g.fillEllipse(cap);
        for(int i=0;i<24;++i){
            float angle=juce::MathConstants<float>::twoPi*float(i)/24.f;
            float sx=std::sin(angle),cy=std::cos(angle);
            g.setColour(juce::Colours::white.withAlpha(.13f));
            g.drawLine(c.x+sx*r*.77f,c.y+cy*r*.77f,c.x+sx*r*.70f,c.y+cy*r*.70f,1);
        }
        g.setColour(juce::Colours::white.withAlpha(.35f));g.drawEllipse(cap,1);
        juce::Path arc;arc.addCentredArc(c.x,c.y,r+3,r+3,0,start,end,true);
        g.setColour(juce::Colour(0xff323641));g.strokePath(arc,juce::PathStrokeType(2));
        juce::Path active;active.addCentredArc(c.x,c.y,r+3,r+3,0,start,start+p*(end-start),true);
        for(int i=5;i>0;--i){g.setColour(neon.withAlpha(.08f));g.strokePath(active,juce::PathStrokeType(float(i)*2+2));}
        g.setColour(neon);g.strokePath(active,juce::PathStrokeType(3.5f));
        g.setColour(juce::Colour(0xffffa4ac));g.strokePath(active,juce::PathStrokeType(1));
        juce::Path pointer;pointer.addRoundedRectangle(-2.f,-r*.64f,4,r*.32f,2.f);
        pointer.applyTransform(juce::AffineTransform::rotation(start+p*(end-start)).translated(c.x,c.y));
        g.setColour(neon);g.strokePath(pointer,juce::PathStrokeType(4));g.setColour(juce::Colour(0xffffe1e5));g.fillPath(pointer);
    }
};
class Editor final : public juce::AudioProcessorEditor,private juce::Timer
{
public:
    explicit Editor(Engine& e):AudioProcessorEditor(e),engine(e) {
        setLookAndFeel(&look);
        
        for(size_t i=0;i<specs.size();++i) {
            auto& s=sliders[i]; s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow,false,82,22);
            s.setTextValueSuffix(specs[i].unit); addAndMakeVisible(s);
            auto& l=labels[i]; l.setText(specs[i].name,juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setColour(juce::Label::textColourId,juce::Colour(0xffd7d7df)); addAndMakeVisible(l);
            attachments[i]=std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(engine.state,specs[i].id,s);
        }
        const char* names[]={"Clean Lead","Melodic Rap","Dark Rap","Intimate R&B","Russ Inspired","Trap Lead","Bright Vocal","Warm R&B","Airy Female","Aggressive Rap","Studio Polish","Wide Hook","Phone Effect","Doubled Lead","Intimate Vocal","Adlib Space","Dry Rap","Smooth Singing","Vintage Warmth","Late Night Hook","Crimson Lead","Midnight Melodic","Noir Drill","Platinum Hook","Redline Rap","Velvet Soul","Ghost Adlibs","Neon Halo","Cinematic Confession","Chrome Presence","After Hours","Inferno Doubles","Rap Presence","Diamond Lead","Crimson Drill","Arena Hook","Silk R&B","Shadow Verse","Red Room Adlibs","Crystal Female"};
        for(int i=0;i<40;++i) presets.addItem(names[i],i+1);
        presets.setTextWhenNothingSelected("SELECT VOCAL PRESET"); addAndMakeVisible(presets);
        advanced.setButtonText("EXPERT"); advanced.setClickingTogglesState(true); addAndMakeVisible(advanced);
        advanced.onClick=[this]{resized(); repaint();};
        const char* titles[]={"INPUT","GATE","HIGH PASS","BODY","AIR","THRESHOLD","RATIO","DE-ESS","DRIVE","DOUBLER","DELAY MIX","DELAY TIME","REVERB MIX","OUTPUT","CEILING"};
        for(size_t i=0;i<specs.size();++i) labels[i].setText(titles[i],juce::dontSendNotification);
        presets.onChange=[this]{ applyPreset(presets.getSelectedId()); };
        bypass.setButtonText("BYPASS"); addAndMakeVisible(bypass);
        bypassAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(engine.state,"bypass",bypass);
        setSize(1120,790); resized(); startTimerHz(24);
    }
    ~Editor() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        const auto red=juce::Colour(0xffff1638);
        g.fillAll(juce::Colour(0xff050506));
        auto metal=[&](float y,float h){
            juce::ColourGradient c(juce::Colour(0xfff8f8fc),0,y,juce::Colour(0xff81838b),0,y+h,false);
            c.addColour(.17,juce::Colour(0xff777981));c.addColour(.37,juce::Colour(0xfff5f5f7));c.addColour(.50,juce::Colour(0xff222328));c.addColour(.54,juce::Colour(0xffa5a7af));c.addColour(.78,juce::Colour(0xff41434b));return c;
        };
        auto cut=[](float x,float y,float w,float h,float d){
            juce::Path p;p.startNewSubPath(x+d,y);p.lineTo(x+w-d,y);p.lineTo(x+w,y+d);p.lineTo(x+w,y+h-d);p.lineTo(x+w-d,y+h);p.lineTo(x+d,y+h);p.lineTo(x,y+h-d);p.lineTo(x,y+d);p.closeSubPath();return p;
        };
        auto rail=[&](float x,float y,float w){
            for(int i=10;i>0;--i){g.setColour(red.withAlpha(.025f+visualEnergy*.015f));g.fillRoundedRectangle(x,y-float(i),w,float(i*2+1),4);}
            g.setColour(red);g.fillRect(x,y,w,2.f);
            g.setGradientFill(juce::ColourGradient(red.withAlpha(0.f),x,y,juce::Colours::white,x+w*.5f,y,false));
            g.fillRect(x,y,w*.5f,1.f);
            g.setGradientFill(juce::ColourGradient(juce::Colours::white,x+w*.5f,y,red.withAlpha(0.f),x+w,y,false));
            g.fillRect(x+w*.5f,y,w*.5f,1.f);
        };
        auto panel=[&](float x,float y,float w,float h){
            auto p=cut(x,y,w,h,16);
            for(int i=6;i>0;--i){g.setColour(red.withAlpha(.045f));g.strokePath(p,juce::PathStrokeType(float(i)*2));}
            g.setGradientFill(metal(y,h));g.fillPath(p);
            auto q=cut(x+3,y+3,w-6,h-6,14);
            juce::ColourGradient dark(juce::Colour(0xff303036),x,y,juce::Colour(0xff080809),x,y+h,false);
            dark.addColour(.18,juce::Colour(0xff101012));dark.addColour(.8,juce::Colour(0xff17171b));g.setGradientFill(dark);g.fillPath(q);
            g.setColour(juce::Colour(0xff82838a));g.strokePath(q,juce::PathStrokeType(.7f));
            rail(x+21,y+4,w-42);rail(x+21,y+h-5,w-42);
            for(float sx:{x+13,x+w-13})for(float sy:{y+13,y+h-13}){g.setColour(juce::Colour(0xffa5a6ae));g.fillEllipse(sx-2.5f,sy-2.5f,5,5);g.setColour(juce::Colours::black);g.drawLine(sx-1.5f,sy+1.5f,sx+1.5f,sy-1.5f);}
        };
        panel(8,8,1104,174);
        g.setFont(juce::FontOptions(92.f,juce::Font::bold|juce::Font::italic));
        for(int i=7;i>0;--i){g.setColour(juce::Colour(0xff24030a));g.drawText("JAM3S",22+i,12+i,505,103,juce::Justification::centred);}
        g.setGradientFill(metal(26,79));g.drawText("JAM3S",22,12,505,103,juce::Justification::centred);
        g.setFont(juce::FontOptions(23.f,juce::Font::bold|juce::Font::italic));g.setColour(red);
        g.drawText("V O C A L   E N G I N E",35,112,478,30,juce::Justification::centred);
        g.setFont(juce::FontOptions(12.f,juce::Font::italic));g.setColour(juce::Colour(0xffc5c5cc));g.drawText("M H   B E A T S   /   V S T 3   /   2 . 4",35,148,478,20,juce::Justification::centred);
        auto meter=[&](int x,const juce::String& label,float value){
            float db=juce::Decibels::gainToDecibels(value,-60.f);
            float level=juce::jlimit(0.f,1.f,(db+60.f)/60.f);
            g.setFont(juce::FontOptions(12.f,juce::Font::bold|juce::Font::italic));g.setColour(juce::Colours::white);g.drawText(label,x,72,120,20,juce::Justification::centred);
            g.setColour(juce::Colour(0xff030304));g.fillRoundedRectangle(float(x),98,120,18,7);
            g.setColour(juce::Colour(0xff74757b));g.drawRoundedRectangle(float(x),98,120,18,7,1);
            for(int i=0;i<29;++i){g.setColour(i<level*29?red:juce::Colour(0xff29292c));g.fillRect(float(x+5+i*3.8f),102.f,2.6f,10.f);}
            g.setColour(juce::Colours::white);g.drawText(juce::String(db,1)+" dB",x,121,120,20,juce::Justification::centred);
        };
        meter(548,"IN LEVEL",visualInput);meter(680,"OUT LEVEL",visualPeak);
        g.setColour(juce::Colours::white);g.setFont(juce::FontOptions(13.f,juce::Font::bold|juce::Font::italic));g.drawText("FACTORY PRESETS  /  40",820,45,270,24,juce::Justification::centredLeft);
        const char* title[]={"TONE","DYNAMICS","CHARACTER","SPACE"};
        const char* sub[]={"FILTER / EQ / SIBILANCE","LEVEL / COMPRESSION","TEXTURE / WIDTH","ECHO / REVERB"};
        for(int k=0;k<4;++k){
            float x=9.f+k*278.f;panel(x,195,268,477);
            g.setGradientFill(metal(214,40));g.setFont(juce::FontOptions(30.f,juce::Font::bold|juce::Font::italic));g.drawText(title[k],int(x)+16,212,236,40,juce::Justification::centredLeft);
            g.setColour(juce::Colour(0xffbcbcc4));g.setFont(juce::FontOptions(11.f,juce::Font::italic));g.drawText(sub[k],int(x)+16,252,236,22,juce::Justification::centredLeft);
            g.setColour(juce::Colour(0xff77777e));g.drawLine(x+13,280,x+255,280,1);
            rail(x+12,282,244);
            for(int j=0;j<3;++j){g.setColour(red.withAlpha(.55f+visualEnergy*.4f));g.drawLine(x+220+j*10,243,x+232+j*10,224,3);}
        }
        panel(9,690,1102,84);
        g.setColour(bypass.getToggleState()?juce::Colour(0xff55555b):red);g.fillEllipse(30,725,10,10);
        g.setFont(juce::FontOptions(13.f,juce::Font::bold|juce::Font::italic));g.setColour(juce::Colours::white);g.drawText(bypass.getToggleState()?"ENGINE BYPASSED":"ENGINE ACTIVE",47,712,150,35,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffbdbdc4));g.drawText("CHROME INFERNO  /  AN ORIGINAL VOCAL TOOL",665,712,420,35,juce::Justification::centredRight);
    }
    void resized() override {
        presets.setBounds(820,78,268,49);advanced.setBounds(215,710,150,40);bypass.setBounds(390,710,150,40);
        for(size_t i=0;i<specs.size();++i){sliders[i].setVisible(false);labels[i].setVisible(false);}
        auto place=[&](int idx,int panel,int col,int count,int y){
            int x=18+panel*278,cell=250/count;
            labels[idx].setBounds(x+col*cell,y,cell,23);
            int w=juce::jmin(cell-3,112);
            sliders[idx].setBounds(x+col*cell+(cell-w)/2,y+25,w,117);
            sliders[idx].setVisible(true);labels[idx].setVisible(true);
        };
        place(2,0,0,3,303);place(3,0,1,3,303);place(4,0,2,3,303);place(7,0,0,2,490);
        place(5,1,0,2,303);place(6,1,1,2,303);
        place(8,2,0,2,303);place(9,2,1,2,303);place(0,2,0,2,490);place(13,2,1,2,490);
        place(11,3,0,2,303);place(10,3,1,2,303);place(12,3,0,2,490);
        if(advanced.getToggleState()){place(1,0,1,2,490);place(14,1,0,2,490);}
    }
private:
    void timerCallback() override {
        visualPeak=juce::jmax(engine.peak.load(),visualPeak*.88f);
        visualInput=juce::jmax(engine.inputPeak.load(),visualInput*.88f);
        visualEnergy+=.20f*(juce::jlimit(0.f,1.f,std::sqrt(visualPeak))-visualEnergy);
        repaint();
    }
    float visualPeak=0.f,visualInput=0.f,visualEnergy=0.f;
    void applyPreset(int id) {
        if(id<1 || id>40) return;
        std::array<float,15> a{};
        for(size_t i=0;i<a.size();++i) a[i]=specs[i].initial;
        if(id==2) { a[5]=-23;a[6]=3.8f;a[7]=35;a[8]=22;a[9]=18;a[10]=17;a[12]=13; }
        if(id==3) { a[3]=2;a[4]=-1;a[5]=-25;a[6]=4.5f;a[8]=30;a[10]=9;a[12]=5; }
        if(id==4) { a[3]=1;a[4]=2.5f;a[5]=-16;a[6]=2.5f;a[7]=35;a[9]=12;a[10]=16;a[12]=20; }
        switch(id){
            case 5: a[3]=1;a[4]=2;a[5]=-22;a[6]=3.5f;a[7]=32;a[8]=17;a[10]=12;a[12]=10;break;
            case 6: a[2]=105;a[4]=2;a[5]=-24;a[6]=4;a[7]=40;a[8]=22;a[12]=5;break;
            case 7: a[3]=-1;a[4]=4;a[7]=40;a[12]=6;break;
            case 8: a[3]=2;a[4]=1;a[5]=-20;a[8]=20;a[12]=18;break;
            case 9: a[2]=120;a[3]=-2;a[4]=4;a[7]=52;a[9]=14;a[12]=22;break;
            case 10:a[5]=-28;a[6]=5;a[8]=40;a[12]=3;break;
            case 11:a[4]=2;a[5]=-19;a[7]=40;a[8]=12;a[12]=7;break;
            case 12:a[5]=-22;a[9]=65;a[10]=25;a[12]=30;break;
            case 13:a[2]=240;a[3]=-9;a[4]=-9;a[5]=-25;a[6]=5;a[8]=60;a[12]=0;break;
            case 14:a[9]=55;a[12]=10;a[10]=0;break;
            case 15:a[5]=-15;a[6]=2;a[8]=8;a[10]=0;a[12]=3;break;
            case 16:a[2]=150;a[9]=55;a[10]=48;a[11]=420;a[12]=60;break;
            case 17:a[5]=-22;a[6]=4;a[10]=0;a[12]=0;break;
            case 18:a[5]=-19;a[6]=2.8f;a[7]=35;a[12]=25;break;
            case 19:a[3]=3;a[4]=-3;a[8]=35;a[12]=12;break;
            case 20:a[3]=1;a[4]=-1;a[9]=42;a[10]=35;a[11]=375;a[12]=32;break;
            case 21: a={{0.0f,-67.0f,92.0f,1.0f,3.0f,-23.0f,4.0f,42.0f,19.0f,0.0f,7.0f,185.0f,5.0f,1.0f,-1.0f}};break;
            case 22: a={{0.0f,-72.0f,82.0f,2.0f,0.0f,-22.0f,3.5f,35.0f,16.0f,23.0f,32.0f,375.0f,24.0f,1.0f,-1.0f}};break;
            case 23: a={{0.0f,-61.0f,105.0f,1.0f,-2.0f,-26.0f,4.8f,38.0f,28.0f,0.0f,5.0f,140.0f,3.0f,1.0f,-1.0f}};break;
            case 24: a={{0.0f,-70.0f,100.0f,-1.0f,4.0f,-24.0f,3.8f,48.0f,12.0f,55.0f,24.0f,310.0f,24.0f,0.0f,-1.0f}};break;
            case 25: a={{0.0f,-62.0f,110.0f,0.0f,2.0f,-28.0f,5.5f,50.0f,35.0f,0.0f,0.0f,120.0f,2.0f,2.0f,-1.0f}};break;
            case 26: a={{0.0f,-76.0f,68.0f,3.0f,1.0f,-16.0f,2.2f,28.0f,12.0f,10.0f,15.0f,280.0f,19.0f,0.0f,-1.0f}};break;
            case 27: a={{0.0f,-72.0f,175.0f,-3.0f,-1.0f,-24.0f,4.0f,35.0f,25.0f,75.0f,55.0f,480.0f,70.0f,-3.0f,-2.0f}};break;
            case 28: a={{0.0f,-75.0f,120.0f,-1.0f,5.0f,-20.0f,3.0f,55.0f,8.0f,40.0f,25.0f,410.0f,40.0f,0.0f,-1.0f}};break;
            case 29: a={{0.0f,-78.0f,75.0f,2.0f,-1.0f,-18.0f,2.5f,25.0f,9.0f,8.0f,18.0f,520.0f,35.0f,0.0f,-1.0f}};break;
            case 30: a={{0.0f,-67.0f,95.0f,-1.0f,3.0f,-21.0f,3.5f,45.0f,14.0f,0.0f,0.0f,180.0f,4.0f,1.0f,-1.0f}};break;
            case 31: a={{0.0f,-74.0f,85.0f,2.0f,-2.0f,-21.0f,3.0f,32.0f,24.0f,30.0f,40.0f,340.0f,28.0f,0.0f,-1.0f}};break;
            case 32: a={{0.0f,-68.0f,140.0f,-2.0f,1.0f,-26.0f,4.5f,42.0f,26.0f,70.0f,12.0f,200.0f,15.0f,-3.0f,-1.0f}};break;
            case 33: a={{0.0f,-68.0f,92.0f,1.0f,2.7f,-24.0f,3.7f,50.0f,19.0f,0.0f,4.0f,185.0f,6.0f,4.0f,-1.0f}};break;
            case 34: a={{0.0f,-72.0f,95.0f,-1.0f,4.0f,-20.0f,3.2f,45.0f,12.0f,0.0f,4.0f,160.0f,5.0f,2.0f,-1.0f}};break;
            case 35: a={{0.0f,-65.0f,110.0f,1.0f,-1.0f,-27.0f,5.0f,40.0f,31.0f,0.0f,6.0f,150.0f,4.0f,2.0f,-1.0f}};break;
            case 36: a={{0.0f,-72.0f,90.0f,1.0f,3.0f,-23.0f,3.5f,45.0f,15.0f,55.0f,30.0f,330.0f,35.0f,1.0f,-1.0f}};break;
            case 37: a={{0.0f,-76.0f,70.0f,2.0f,2.0f,-17.0f,2.4f,32.0f,12.0f,15.0f,15.0f,270.0f,20.0f,1.0f,-1.0f}};break;
            case 38: a={{0.0f,-70.0f,85.0f,3.0f,-3.0f,-22.0f,3.8f,32.0f,25.0f,0.0f,12.0f,230.0f,8.0f,1.0f,-1.0f}};break;
            case 39: a={{0.0f,-74.0f,160.0f,-3.0f,1.0f,-25.0f,4.0f,45.0f,30.0f,65.0f,45.0f,410.0f,55.0f,-3.0f,-2.0f}};break;
            case 40: a={{0.0f,-78.0f,120.0f,-2.0f,5.0f,-18.0f,2.8f,55.0f,8.0f,12.0f,10.0f,220.0f,18.0f,1.0f,-1.0f}};break;
            default: break;
        }
        for(size_t i=0;i<a.size();++i) if(auto* p=engine.state.getParameter(specs[i].id)) {
            p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(a[i])); p->endChangeGesture();
        }
    }
    Engine& engine; ChromeLook look;
    std::array<juce::Slider,15> sliders;
    std::array<juce::Label,15> labels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,15> attachments;
    juce::ComboBox presets; juce::ToggleButton bypass, advanced;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
};
juce::AudioProcessorEditor* Engine::createEditor(){return new Editor(*this);}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Engine();}
