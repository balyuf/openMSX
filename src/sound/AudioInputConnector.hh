#ifndef AUDIOINPUTCONNECTOR_HH
#define AUDIOINPUTCONNECTOR_HH

#include "Connector.hh"
#include <cstdint>

namespace openmsx {

class AudioInputDevice;

class AudioInputConnector final : public Connector
{
public:
	AudioInputConnector(PluggingController& pluggingController,
	                    std::string name);

	AudioInputDevice& getPluggedAudioDev() const;

	// Connector
	std::string_view getDescription() const final override;
	std::string_view getClass() const final override;

	int16_t readSample(EmuTime::param time) const;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);
};

} // namespace openmsx

#endif
