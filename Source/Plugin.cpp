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
        const char* names[]={"Clean Lead","Melodic Rap","Dark Rap","Intimate R&B","Russ Inspired","Trap Lead","Bright Vocal","Warm R&B","Airy Female","Aggressive Rap","Studio Polish","Wide Hook","Phone Effect","Doubled Lead","Intimate Vocal","Adlib Space","Dry Rap","Smooth Singing","Vintage Warmth","Late Night Hook"};
        for(int i=0;i<20;++i) presets.addItem(names[i],i+1);
        presets.setTextWhenNothingSelected("SELECT VOCAL PRESET"); addAndMakeVisible(presets);
        advanced.setButtonText("ADVANCED"); advanced.setClickingTogglesState(true); addAndMakeVisible(advanced);
        advanced.onClick=[this]{resized(); repaint();};
        const char* titles[]={"INPUT","NOISE","LOW CUT","TONE","CLARITY","COMPRESS","RATIO","DE-ESS","WARMTH","WIDTH","DELAY","TIME","REVERB","OUTPUT","CEILING"};
        for(size_t i=0;i<specs.size();++i) labels[i].setText(titles[i],juce::dontSendNotification);
        presets.onChange=[this]{ applyPreset(presets.getSelectedId()); };
        bypass.setButtonText("BYPASS"); addAndMakeVisible(bypass);
        bypassAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(engine.state,"bypass",bypass);
        setSize(1100,610); resized(); startTimerHz(24);
    }
    ~Editor() override { stopTimer(); setLookAndFeel(nullptr); }
    void paint(juce::Graphics& g) override {
        const auto red=juce::Colour(0xffff123c);
        g.fillAll(juce::Colour(0xff06070a));
        juce::ColourGradient steel(juce::Colour(0xffb5b8c2),0,0,juce::Colour(0xff31343d),0,610,false);
        steel.addColour(.10,juce::Colour(0xfff0f4fa));steel.addColour(.18,juce::Colour(0xff4b5260));steel.addColour(.48,juce::Colour(0xffa9b2bf));steel.addColour(.51,juce::Colour(0xff13151b));steel.addColour(.93,juce::Colour(0xffe1e7ef));
        g.setGradientFill(steel);g.fillRoundedRectangle(4,4,1092,602,20);
        g.setColour(juce::Colour(0xff08090d));g.fillRoundedRectangle(11,11,1078,588,15);
        for(int i=0;i<6;++i){g.setColour(red.withAlpha(.07f));g.drawRoundedRectangle(12.f+i,12.f+i,1076.f-2*i,586.f-2*i,14,3);}
        g.setColour(red);g.drawRoundedRectangle(13,13,1074,584,14,1.5f);
        auto plate=[&](float x,float y,float w,float h){
            g.setColour(juce::Colours::black.withAlpha(.65f));g.fillRoundedRectangle(x+2,y+5,w,h,12);
            juce::ColourGradient bevel(juce::Colour(0xffe1e6ee),x,y,juce::Colour(0xff606875),x,y+h,false);bevel.addColour(.14,juce::Colour(0xff333a45));bevel.addColour(.83,juce::Colour(0xff15191f));g.setGradientFill(bevel);g.fillRoundedRectangle(x,y,w,h,12);
            juce::ColourGradient p(juce::Colour(0xff30333d),x,y,juce::Colour(0xff0c0e13),x,y+h,false);
            p.addColour(.22,juce::Colour(0xff17191f));g.setGradientFill(p);g.fillRoundedRectangle(x+3,y+3,w-6,h-6,9);
            g.setColour(juce::Colour(0xff727987));g.drawRoundedRectangle(x+3,y+3,w-6,h-6,9,1);
            for(int i=0;i<4;++i){g.setColour(red.withAlpha(.07f));g.drawLine(x+12,y+5+i,x+w-12,y+5+i,2);}
            g.setColour(red.withAlpha(.65f));g.drawLine(x+14,y+5,x+w-14,y+5,1);
        };
        plate(22,22,250,566);plate(285,22,793,96);plate(285,130,793,260);plate(285,402,793,186);
        for(int i=0;i<6;++i){g.setColour(red.withAlpha(.04f));g.drawRoundedRectangle(14.f+i,14.f+i,1072.f-2*i,582.f-2*i,14,3);}
        for(int i=8;i>0;--i){g.setColour(red.withAlpha(.045f));g.fillRoundedRectangle(286,120.f-i,790,float(2*i+2),4);}
        g.setColour(red);g.fillRoundedRectangle(286,119,790,3,1);g.setColour(juce::Colour(0xffffa1ad));g.fillRect(300,120,762,1);
        g.setFont(juce::FontOptions(55.f,juce::Font::bold|juce::Font::italic));
        juce::ColourGradient logo(juce::Colour(0xfffafaff),0,40,juce::Colour(0xff757a85),0,102,false);
        logo.addColour(.5,juce::Colour(0xffbcc0ca));g.setGradientFill(logo);
        for(int i=5;i>0;--i){g.setColour(juce::Colour(0xff090a0d));g.drawText("JAM3S",32+i,38+i,228,72,juce::Justification::centred);}
        g.setColour(red.withAlpha(.35f));g.drawText("JAM3S",32,40,228,72,juce::Justification::centred);
        g.setGradientFill(logo);g.drawText("JAM3S",32,38,228,72,juce::Justification::centred);
        g.setFont(juce::FontOptions(13.f,juce::Font::bold));g.setColour(red);
        g.drawText("V O C A L   E N G I N E",30,109,234,24,juce::Justification::centred);
        g.setColour(juce::Colour(0xffa3a7b4));g.drawText("CHROME INFERNO  /  2.2",30,134,234,22,juce::Justification::centred);
        g.setColour(juce::Colour(0xff090a0e));g.fillRoundedRectangle(42,177,210,230,8);
        g.setColour(red);g.setFont(juce::FontOptions(12.f,juce::Font::bold));g.drawText("SIGNAL OUTPUT",52,184,190,25,juce::Justification::centred);
        float db=juce::Decibels::gainToDecibels(engine.peak.load(),-60.f);
        float level=juce::jlimit(0.f,1.f,(db+60.f)/60.f);
        for(int i=0;i<28;++i){if(i<level*28){g.setColour(red.withAlpha(.13f));g.fillRoundedRectangle(72,363.f-i*5.2f,150,7,3);}
            g.setColour(i<level*28?red:juce::Colour(0xff262831));g.fillRoundedRectangle(78,365.f-i*5.2f,138,3.5f,1);
            if(i<level*28){g.setColour(juce::Colour(0xffff9baa));g.fillRect(80.f,365.f-i*5.2f,134.f,.8f);}}
        g.setColour(juce::Colours::white);g.setFont(juce::FontOptions(18.f));g.drawText(juce::String(db,1)+" dB",52,375,190,25,juce::Justification::centred);
        g.setFont(juce::FontOptions(12.f,juce::Font::bold));g.setColour(juce::Colour(0xffa9adba));
        g.drawText("PRESET BANK  /  20",42,423,210,20,juce::Justification::centredLeft);
        g.drawText("01   /   VOCAL CONTROL",310,141,710,25,juce::Justification::centredLeft);
        g.drawText("02   /   CHARACTER & SPACE",310,410,710,25,juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(27.f,juce::Font::bold));g.setColour(juce::Colours::white);
        g.drawText(advanced.getToggleState()?"ENGINE / EXPERT":"ENGINE / ESSENTIAL",310,38,470,40,juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(13.f));g.setColour(juce::Colour(0xffa9adba));g.drawText("PRO VOCAL CHANNEL STRIP",312,79,470,20,juce::Justification::centredLeft);
        for(int x: {30,1070})for(int y:{30,580}){g.setColour(juce::Colour(0xff757986));g.fillEllipse(float(x-3),float(y-3),6,6);g.setColour(juce::Colours::black);g.drawLine(float(x-2),float(y+2),float(x+2),float(y-2));}
    }
    void resized() override {
        presets.setBounds(42,449,210,40);advanced.setBounds(42,510,210,30);bypass.setBounds(42,548,210,28);
        for(size_t i=0;i<specs.size();++i){sliders[i].setVisible(false);labels[i].setVisible(false);}
        auto place=[&](int index,int col,int count,int y,int h){
            int cell=748/count,x=307+col*cell;
            labels[index].setBounds(x,y,cell,24);
            int w=juce::jmin(cell-6,h);
            sliders[index].setBounds(x+(cell-w)/2,y+25,w,h);
            sliders[index].setVisible(true);labels[index].setVisible(true);
        };
        if(advanced.getToggleState()){
            const int top[]={0,1,2,3,4,5,6,7};const int bottom[]={8,9,10,11,12,13,14};
            for(int i=0;i<8;++i)place(top[i],i,8,195,140);
            for(int i=0;i<7;++i)place(bottom[i],i,7,440,112);
        }else{
            const int top[]={0,3,4,5,7,13};const int bottom[]={8,9,10,12};
            for(int i=0;i<6;++i)place(top[i],i,6,175,184);
            for(int i=0;i<4;++i)place(bottom[i],i,4,440,116);
        }
    }
private:
    void timerCallback() override { repaint(42,177,210,230); }
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
