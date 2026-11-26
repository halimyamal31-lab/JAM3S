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
        g.setColour(juce::Colour(0xff08090b));g.fillEllipse(ring.expanded(2));
        for(int i=0;i<5;++i){g.setColour(juce::Colour(0xffff2040).withAlpha(.035f));g.drawEllipse(ring.expanded(float(i)),2);}
        juce::ColourGradient edge(juce::Colour(0xffe5e7ed),c.x,c.y-r,juce::Colour(0xff444650),c.x,c.y+r,false);
        edge.addColour(.42,juce::Colour(0xff33343c));edge.addColour(.53,juce::Colour(0xffa3a6b1));g.setGradientFill(edge);g.fillEllipse(ring);
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
        setSize(980,570);
        for(size_t i=0;i<specs.size();++i) {
            auto& s=sliders[i]; s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow,false,82,22);
            s.setTextValueSuffix(specs[i].unit); addAndMakeVisible(s);
            auto& l=labels[i]; l.setText(specs[i].name,juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setColour(juce::Label::textColourId,juce::Colour(0xffd7d7df)); addAndMakeVisible(l);
            attachments[i]=std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(engine.state,specs[i].id,s);
        }
        const char* names[]={"Clean Lead","Melodic Rap","Dark Rap","Intimate R&B","Russ Inspired","Trap Lead","Bright Vocal","Warm R&B","Airy Female","Aggressive Rap","Studio Polish","Wide Hook","Phone Effect","Doubled Lead","Intimate Vocal","Adlib Space","Dry Rap","Smooth Singing","Vintage Warmth","Late Night Hook"};
        for(int i=0;i<20;++i) presets.addItem(names[i],i+1);
        presets.setTextWhenNothingSelected("SELECT VOCAL PRESET"); addAndMakeVisible(presets);
        advanced.setButtonText("ADVANCED"); addAndMakeVisible(advanced);
        advanced.onClick=[this]{resized(); repaint();};
        const char* titles[]={"INPUT","NOISE","LOW CUT","TONE","CLARITY","COMPRESS","RATIO","DE-ESS","WARMTH","WIDTH","DELAY","TIME","REVERB","OUTPUT","CEILING"};
        for(size_t i=0;i<specs.size();++i) labels[i].setText(titles[i],juce::dontSendNotification);
        presets.onChange=[this]{ applyPreset(presets.getSelectedId()); };
        bypass.setButtonText("BYPASS"); addAndMakeVisible(bypass);
        bypassAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(engine.state,"bypass",bypass);
        startTimerHz(24);
    }
    ~Editor() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff08090c));
        auto frame=getLocalBounds().toFloat().reduced(5);
        juce::ColourGradient metal(juce::Colour(0xff777981),0,0,juce::Colour(0xff18191e),0,570,false);
        metal.addColour(.12,juce::Colour(0xff101115)); metal.addColour(.50,juce::Colour(0xff3d3e46));
        metal.addColour(.52,juce::Colour(0xff101115)); g.setGradientFill(metal); g.fillRoundedRectangle(frame,16);
        g.setColour(juce::Colour(0xff07080b)); g.fillRoundedRectangle(frame.reduced(5),12);
        for(int i=0;i<8;++i) {g.setColour(juce::Colour(0xffff163e).withAlpha(.025f));g.drawRoundedRectangle(frame.reduced(float(6+i)),12,2);}
        g.setColour(juce::Colour(0xfffd2544));g.drawRoundedRectangle(frame.reduced(7),11,1.5f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff34363e),0,12,juce::Colour(0xff0c0d11),0,112,false));
        g.fillRoundedRectangle(17,17,946,98,10);
        g.setFont(juce::FontOptions(48.0f,juce::Font::bold|juce::Font::italic));
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xfff4f6ff),0,22,juce::Colour(0xff717681),0,72,false));
        g.drawText("JAM3S",33,17,300,61,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffff3150));g.setFont(juce::FontOptions(12.0f,juce::Font::bold));
        g.drawText("V O C A L   E N G I N E   /   V 2",36,77,380,20,juce::Justification::centredLeft);
        auto panel=[&](float y,float h,const juce::String& title){
            juce::Rectangle<float> r(22,y,936,h);
            juce::ColourGradient grad(juce::Colour(0xff202127),0,y,juce::Colour(0xff101116),0,y+h,false);
            g.setGradientFill(grad);g.fillRoundedRectangle(r,10);
            g.setColour(juce::Colour(0xff484951));g.drawRoundedRectangle(r,10,1);
            g.setColour(juce::Colour(0xffff2847));g.fillRoundedRectangle(35,y+12,22,3,1);
            g.setColour(juce::Colour(0xffbfc2cc));g.setFont(juce::FontOptions(12.0f,juce::Font::bold));
            g.drawText(title,65,int(y)+5,850,20,juce::Justification::centredLeft);
        };
        panel(129,206,advanced.getToggleState()?"ADVANCED  /  SIGNAL SHAPING":"01  /  VOCAL SHAPE");
        panel(346,170,advanced.getToggleState()?"ADVANCED  /  EFFECTS & OUTPUT":"02  /  COLOR & SPACE");
        g.setColour(juce::Colour(0xff30323a));g.fillRoundedRectangle(35,537,750,12,4);
        float level=juce::jlimit(0.f,1.f,engine.peak.load());
        for(int i=0;i<50;++i){
            g.setColour(i<level*50?juce::Colour(0xffff304a):juce::Colour(0xff191a20));
            g.fillRoundedRectangle(37.f+i*14.9f,539,12,8,1);
        }
        g.setColour(juce::Colour(0xffbfc2cc));g.setFont(juce::FontOptions(12.0f));
        g.drawText("OUTPUT  /  VST3",800,530,145,25,juce::Justification::centredRight);
    }
    void resized() override {
        presets.setBounds(385,33,325,37); advanced.setBounds(725,33,116,37); bypass.setBounds(850,33,95,37);
        for(size_t i=0;i<specs.size();++i){sliders[i].setVisible(false);labels[i].setVisible(false);}
        auto place=[&](int index,int col,int count,int y,int height){
            int cell=900/count,x=40+col*cell;
            labels[index].setBounds(x,y,cell,22);
            int w=juce::jmin(cell-10,height-12);
            sliders[index].setBounds(x+(cell-w)/2,y+22,w,height);
            sliders[index].setVisible(true);labels[index].setVisible(true);
        };
        if(advanced.getToggleState()){
            const int top[]={0,1,2,3,4,5,6,7};const int bottom[]={8,9,10,11,12,13,14};
            for(int i=0;i<8;++i)place(top[i],i,8,164,145);
            for(int i=0;i<7;++i)place(bottom[i],i,7,377,110);
        }else{
            const int top[]={0,3,4,5,7,13};const int bottom[]={8,9,10,12};
            for(int i=0;i<6;++i)place(top[i],i,6,164,145);
            for(int i=0;i<4;++i)place(bottom[i],i,4,377,110);
        }
    }
private:
    void timerCallback() override { repaint(0,526,getWidth(),40); }
    void applyPreset(int id) {
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
