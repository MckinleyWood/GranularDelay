#include "PluginProcessor.h"
#include "PluginEditor.h"

/*

TO-DO:

    • Change playback speed to be applied after grains are created?
    • Change slider sticking conditions to reflect this
*/

//==============================================================================
GranularDelayAudioProcessor::GranularDelayAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput  ("Input",  juce::AudioChannelSet::stereo())
                      .withOutput ("Output", juce::AudioChannelSet::stereo())),
                      apvts(*this, nullptr, "Parameters", createParameterLayout()),
                      waveViewer(1), timer([this]() { timerCallback(); })
{
}

GranularDelayAudioProcessor::~GranularDelayAudioProcessor()
{
}

//==============================================================================
const juce::String GranularDelayAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool GranularDelayAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool GranularDelayAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool GranularDelayAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double GranularDelayAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int GranularDelayAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int GranularDelayAudioProcessor::getCurrentProgram()
{
    return 0;
}

void GranularDelayAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String GranularDelayAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void GranularDelayAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void GranularDelayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);

    auto delayBufferSize = static_cast<int>(sampleRate * 10);
    delayBuffer.setSize(getTotalNumOutputChannels(), delayBufferSize);
    wetBuffer.setSize(getTotalNumInputChannels(), samplesPerBlock);

    waveViewer.setSamplesPerBlock(delayBufferSize / 2 / 1024);

    timer.startTimer(timerInterval);

    DBG("Plugin set up!");
}

void GranularDelayAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool GranularDelayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void GranularDelayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    DBG("Processing a block!");
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;

    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    auto mainBufferSize = buffer.getNumSamples();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Get parameter values
    auto chainSettings = getChainSettings(apvts);
    float inputGain = chainSettings.inputGain;
    float mix = chainSettings.mix;
    float frequency = chainSettings.frequency;

    int interval = static_cast<int>(1000 / frequency);
    timerInterval = interval;

    if (grainRequested.load())
    {
        DBG("Grain requested!");
        addGrain();
        DBG("Grain added!");
        grainRequested.store(false);
    }

    wetBuffer.clear();
    buffer.applyGain(inputGain);

    // Give the buffer to the wave viewer 
    waveViewer.pushBuffer(buffer);

    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {        
        // Copy the input buffer into the delayBuffer
        fillDelayBuffer(buffer, channel, 1.f);

        // Read from the grains into the wetBuffer
        readGrains(wetBuffer, channel);
        
        // Mix grains with dry signal 
        buffer.applyGain(channel, 0, mainBufferSize, 1.f - mix);
        buffer.addFrom(channel, 0, wetBuffer, channel, 0, mainBufferSize, mix);
    }

    updateWritePosition(mainBufferSize);
}

// Copies one channel of a buffer to the delayBuffer at the writePosition (with wraparound)
void GranularDelayAudioProcessor::fillDelayBuffer(juce::AudioBuffer<float>& buffer, int channel, float gain)
{   
    auto bufferSize = buffer.getNumSamples();
    auto delayBufferSize = delayBuffer.getNumSamples();
    // DBG("bufferSize = " << bufferSize);

    // Check if there is enough room for full buffer in delayBuffer
    if (delayBufferSize >= bufferSize + writePosition)
    {
        // If so, copy entire buffer over
        delayBuffer.copyFrom(channel, writePosition, buffer.getWritePointer(channel), bufferSize, gain);
    }
    else
    {
        // Determine how much space is left at the end / how much to put at start
        auto numSamplesToEnd = delayBufferSize - writePosition;
        auto numSamplesLeft = bufferSize - numSamplesToEnd;

        // Copy samples to end / start
        delayBuffer.copyFrom(channel, writePosition, buffer.getWritePointer(channel), numSamplesToEnd, gain);
        delayBuffer.copyFrom(channel, 0, buffer.getWritePointer(channel, numSamplesToEnd), numSamplesLeft, gain);
    }    
}

// Reads from all of the grains in the grainVector into the given buffer
void GranularDelayAudioProcessor::readGrains(juce::AudioBuffer<float>& buffer, int channel)
{
    if (!grainVector.empty())
    {
        DBG("Reading grains...");
        DBG("grainVector Size = " << grainVector.size());

        int mainBufferSize = buffer.getNumSamples();
        for (size_t i = grainVector.size(); i-- > 0;)
        {
            DBG("Processing grain #" << i);

            int grainBufferSize = grainVector[i].buffer.getNumSamples();
            int readPosition = grainVector[i].readPosition;
            int samplesToCopy = juce::jmin(grainBufferSize - readPosition, mainBufferSize);

            buffer.addFrom(channel, 0, grainVector[i].buffer, channel, readPosition, samplesToCopy, 0.7f);

            if (grainBufferSize - readPosition > mainBufferSize)
            {
                grainVector[i].readPosition += mainBufferSize;
            }    
            else
            {
                grainVector.erase(grainVector.begin() + i);
                DBG("Grain removed!");
            }
        }
    }
}

// Updates the write position of the delay buffer (after processing a block)
void GranularDelayAudioProcessor::updateWritePosition(int numSamples)
{
    writePosition += numSamples;
    writePosition %= delayBuffer.getNumSamples();
}


//==============================================================================
// Adds a new grain to the grainVector, taking samples from the delayBuffer
void GranularDelayAudioProcessor::addGrain()
{
    int sampleRate = static_cast<int>(getSampleRate());
    auto chainSettings = getChainSettings(apvts);
    float grainSize = chainSettings.grainSize;
    int grainSizeSamples = static_cast<int>(grainSize * sampleRate / 1000);

    float startSample = getGrainStartSample();
    float pitch = getGrainPitch();
	
    // Copy audio from delayBuffer to new AudioBuffer
	auto grainBuffer = juce::AudioBuffer<float>(2, grainSizeSamples);
    grainBuffer.clear();

    for (int channel = 0; channel < getTotalNumInputChannels(); ++channel)
    {
        fillGrainBuffer(grainBuffer, channel, startSample, pitch);
    }
    
    // Apply fade envelope to the grain buffer
    int fadeLengthSamples = grainSizeSamples / 5;
    grainBuffer.applyGainRamp(0, fadeLengthSamples, 0, 1);
    grainBuffer.applyGainRamp(grainSizeSamples - fadeLengthSamples, fadeLengthSamples, 1, 0);

    // Initialize Grain object and add it to the vector
	Grain grain;
    grain.buffer = grainBuffer;
    grain.readPosition = 0;

    grainVector.push_back(grain);
}

// Returns a random start sample within the bounds set by the rangeStart and rangeEnd parameters
float GranularDelayAudioProcessor::getGrainStartSample()
{
    int sampleRate = static_cast<int>(getSampleRate());
    int delayBufferSize = delayBuffer.getNumSamples();
    auto chainSettings = getChainSettings(apvts);
    float rangeStart = chainSettings.rangeStart;
    float rangeEnd = chainSettings.rangeEnd;

    int rangeStartSamples = static_cast<int>(rangeStart * sampleRate / 1000.f);
    int rangeEndSamples = static_cast<int>(rangeEnd * sampleRate / 1000.f);

    int maxStartSample = writePosition - rangeStartSamples + delayBufferSize;
	int minStartSample = writePosition - rangeEndSamples + delayBufferSize;

    jassert(minStartSample <= maxStartSample); 

    float startSample = minStartSample;
    if (minStartSample != maxStartSample)
    {
        juce::Random random;
        startSample = juce::jmap(random.nextFloat(), 0.0f, 1.0f, 
                                 static_cast<float>(minStartSample),
                                 static_cast<float>(maxStartSample));
    }

    if (startSample > delayBufferSize)
            startSample -= delayBufferSize;

    return startSample;
}

// Returns a random pitch / playback speed value within the range set by the pitch and detune parameters
float GranularDelayAudioProcessor::getGrainPitch()
{
    auto chainSettings = getChainSettings(apvts);
    float grainPitch = chainSettings.grainPitch;
    float detune = chainSettings.detune;
    float pitch;

    if (juce::approximatelyEqual<float>(detune, 0.f))
    {
        pitch = grainPitch;
    }
    else
    {
        float detuneFactor = std::pow(2.0f, detune / 1200.0f); 

        float minPitch = grainPitch * detuneFactor;
        float maxPitch = grainPitch / detuneFactor;

        juce::Random random;
        pitch = juce::jmap(random.nextFloat(), 0.0f, 1.0f, minPitch, maxPitch);
    }
    
    return pitch;
}

void GranularDelayAudioProcessor::fillGrainBuffer(juce::AudioBuffer<float>& grainBuffer, 
                                                  int channel, float startSample, float pitch)
{
    float delayBufferReadPosition = startSample;
    int grainBufferSize = grainBuffer.getNumSamples();
    int delayBufferSize = delayBuffer.getNumSamples();
    for (int i = 0; i < grainBufferSize; ++i)
    {
        int truncatedPos = static_cast<int>(delayBufferReadPosition);
        float fraction = delayBufferReadPosition - truncatedPos;

        jassert(truncatedPos < delayBuffer.getNumSamples());
        float sample1 = delayBuffer.getSample(channel, truncatedPos);
        float sample2 = delayBuffer.getSample(channel, (truncatedPos + 1) % delayBufferSize); 
        // mod handles the edge case where we need to interpolate between the last and first samples of delayBuffer
        float interpolatedSample = sample1 * (1 - fraction) + sample2 * fraction;

        grainBuffer.setSample(channel, i, interpolatedSample);

        delayBufferReadPosition += pitch;
        if (delayBufferReadPosition >= delayBufferSize)
            delayBufferReadPosition -= delayBufferSize;
    }
}


//==============================================================================
void GranularDelayAudioProcessor::timerCallback()
{
    grainRequested.store(true);
    timer.startTimer(timerInterval);
}


//==============================================================================
bool GranularDelayAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* GranularDelayAudioProcessor::createEditor()
{
    return new GranularDelayAudioProcessorEditor (*this); // Use custom editor
    //return new juce::GenericAudioProcessorEditor (*this); // Use default editor
}

//==============================================================================
void GranularDelayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ignoreUnused (destData);

    // Write parameters to memory block
    juce::MemoryOutputStream mos(destData, true);
    apvts.state.writeToStream(mos);
}

void GranularDelayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ignoreUnused (data, sizeInBytes);

    // Restore parameters from memory block
    auto tree = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes));
    if (tree.isValid()) 
    {
        apvts.replaceState(tree);
    }
}

ChainSettings getChainSettings(juce::AudioProcessorValueTreeState& apvts)
{
    ChainSettings settings;

    settings.inputGain = apvts.getRawParameterValue("inputGain")->load();
    settings.mix = apvts.getRawParameterValue("mix")->load();
    settings.grainSize = apvts.getRawParameterValue("grainSize")->load();
    settings.frequency = apvts.getRawParameterValue("frequency")->load();
    settings.rangeStart = apvts.getRawParameterValue("rangeStart")->load();
    settings.rangeEnd = apvts.getRawParameterValue("rangeEnd")->load();
    settings.grainPitch = apvts.getRawParameterValue("grainPitch")->load();
    settings.detune = apvts.getRawParameterValue("detune")->load();
    settings.dummy2 = apvts.getRawParameterValue("dummy2")->load();
    settings.dummy4 = apvts.getRawParameterValue("dummy4")->load();

    return settings;
}

juce::AudioProcessorValueTreeState::ParameterLayout
    GranularDelayAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Add parameters to the APVTS layout
    layout.add(std::make_unique<juce::AudioParameterFloat>("inputGain", "Input Gain", 0.f, 2.f, 1.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("mix", "Mix", 0.f, 1.f, 0.5f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("grainSize", "Grain Size", 
                                juce::NormalisableRange<float>(1.f, 100.f, 0.f, 1.f), 50.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("frequency", "Frequency", 
                                juce::NormalisableRange<float>(1.f, 100.f, 0.f, 1.f), 50.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("rangeStart", "Range Start",
                                juce::NormalisableRange<float>(0.f, 5000.f, 0.f, 0.58f), 500.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("rangeEnd", "Range End",
                                juce::NormalisableRange<float>(0.f, 5000.f, 0.f, 0.58f), 1500.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("grainPitch", "Pitch/Speed", 
                                juce::NormalisableRange<float>(0.25f, 4.f, 0.f, 0.43f), 1.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("detune", "Detune",
                                juce::NormalisableRange<float>(0.f, 500.f, 0.f, 0.5f), 0.f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("dummy2", "dummy2", 0.f, 1.f, 0.5f));

    layout.add(std::make_unique<juce::AudioParameterFloat>("dummy4", "dummy4", 0.f, 1.f, 0.5f));

    return layout;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GranularDelayAudioProcessor();
}

