#include "DeltaProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeltaProcessor();
}
