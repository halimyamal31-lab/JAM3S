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
    std::atomic<float> peak {0};
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
        if (value("bypass")>0.5f || history.getNumSamples()<2) return;
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
        auto box=juce::Rectangle<float>(float(x),float(y),float(w),float(h)).reduced(7);
        float r=juce::jmin(box.getWidth(),box.getHeight())*.5f;
        auto c=box.getCentre();
        auto ring=juce::Rectangle<float>(2*r,2*r).withCentre(c);
        g.setColour(juce::Colour(0xff303039)); g.fillEllipse(ring);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff666671),c.x,c.y-r,
            juce::Colour(0xff08080b),c.x,c.y+r,false));
        g.fillEllipse(ring.reduced(4));
        g.setColour(juce::Colour(0xff050507)); g.drawEllipse(ring.reduced(4),2);
        juce::Path arc; arc.addCentredArc(c.x,c.y,r-1,r-1,0,start,end,true);
        g.setColour(juce::Colour(0xff50505a)); g.strokePath(arc,juce::PathStrokeType(3));
        juce::Path active; active.addCentredArc(c.x,c.y,r-1,r-1,0,start,start+p*(end-start),true);
        g.setColour(juce::Colour(0xffff334d)); g.strokePath(active,juce::PathStrokeType(3.5f));
        juce::Path pointer; pointer.addRoundedRectangle(-1.5f,-r*.69f,3,r*.34f,1.5f);
        pointer.applyTransform(juce::AffineTransform::rotation(start+p*(end-start)).translated(c.x,c.y));
        g.setColour(juce::Colour(0xffffc5cb)); g.fillPath(pointer);
    }
};
class Editor final : public juce::AudioProcessorEditor,private juce::Timer
{
public:
    explicit Editor(Engine& e):AudioProcessorEditor(e),engine(e) {
        setLookAndFeel(&look);
        setSize(900,590);
        for(size_t i=0;i<specs.size();++i) {
            auto& s=sliders[i]; s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow,false,82,22);
            s.setTextValueSuffix(specs[i].unit); addAndMakeVisible(s);
            auto& l=labels[i]; l.setText(specs[i].name,juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setColour(juce::Label::textColourId,juce::Colour(0xffd7d7df)); addAndMakeVisible(l);
            attachments[i]=std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(engine.state,specs[i].id,s);
        }
        presets.addItem("Clean Lead",1); presets.addItem("Melodic Rap",2);
        presets.addItem("Dark Rap",3); presets.addItem("Intimate R&B",4);
        presets.setSelectedId(1,juce::dontSendNotification); addAndMakeVisible(presets);
        presets.onChange=[this]{ applyPreset(presets.getSelectedId()); };
        bypass.setButtonText("BYPASS"); addAndMakeVisible(bypass);
        bypassAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(engine.state,"bypass",bypass);
        startTimerHz(24);
    }
    ~Editor() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff0b0b10));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff303039),0,0,juce::Colour(0xff0a0a0e),0,110,false));
        g.fillRect(0,0,getWidth(),104);
        g.setColour(juce::Colour(0xffff3049)); g.fillRect(0,101,getWidth(),3);
        g.setFont(juce::FontOptions(43.0f,juce::Font::bold));
        g.drawText("JAM3S",28,8,300,55,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffbdbdc9)); g.setFont(juce::FontOptions(13.0f));
        g.drawText("V O C A L   E N G I N E",30,62,340,24,juce::Justification::centredLeft);
        for(int row=0;row<3;++row) {
            auto rect=juce::Rectangle<float>(20,float(119+row*144),860,136);
            g.setColour(juce::Colour(0xff17171e)); g.fillRoundedRectangle(rect,9);
            g.setColour(juce::Colour(0xff35353f)); g.drawRoundedRectangle(rect,9,1);
        }
        g.setColour(juce::Colour(0xff3a3a43)); g.fillRoundedRectangle(24,565,680,8,3);
        g.setColour(juce::Colour(0xffff3049)); g.fillRoundedRectangle(24,565,680*juce::jlimit(0.f,1.f,engine.peak.load()),8,3);
        g.setColour(juce::Colour(0xffc1c1cc)); g.setFont(juce::FontOptions(12.0f));
        g.drawText("OUTPUT   /   VST3",720,554,150,28,juce::Justification::centredRight);
    }
    void resized() override {
        presets.setBounds(540,25,220,32); bypass.setBounds(775,25,100,32);
        for(size_t i=0;i<specs.size();++i) {
            int x=30+int(i%5)*170,y=124+int(i/5)*144;
            labels[i].setBounds(x,y,160,23); sliders[i].setBounds(x+18,y+22,124,104);
        }
    }
private:
    void timerCallback() override { repaint(0,550,getWidth(),40); }
    void applyPreset(int id) {
        std::array<float,15> a{};
        for(size_t i=0;i<a.size();++i) a[i]=specs[i].initial;
        if(id==2) { a[5]=-23;a[6]=3.8f;a[7]=35;a[8]=22;a[9]=18;a[10]=17;a[12]=13; }
        if(id==3) { a[3]=2;a[4]=-1;a[5]=-25;a[6]=4.5f;a[8]=30;a[10]=9;a[12]=5; }
        if(id==4) { a[3]=1;a[4]=2.5f;a[5]=-16;a[6]=2.5f;a[7]=35;a[9]=12;a[10]=16;a[12]=20; }
        for(size_t i=0;i<a.size();++i) if(auto* p=engine.state.getParameter(specs[i].id)) {
            p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(a[i])); p->endChangeGesture();
        }
    }
    Engine& engine; ChromeLook look;
    std::array<juce::Slider,15> sliders;
    std::array<juce::Label,15> labels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,15> attachments;
    juce::ComboBox presets; juce::ToggleButton bypass;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
};
juce::AudioProcessorEditor* Engine::createEditor(){return new Editor(*this);}
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){return new Engine();}
