#pragma once

#include <string>
#include <utility>
#include <vector>

class Config {
public:
	Config();
	~Config();

	bool RenderCustomHands() const { return renderCustomHands; }
	vr::HmdColor_t HandColour() const { return handColour; }
	float SupersampleRatio() const { return supersampleRatio; }

	/**
	 * supersampleRatio for the given OpenXR runtime: from the first ini section whose name occurs in runtimeName
	 * (case-insensitive), e.g. [Oculus] for Meta's runtime, else the global supersampleRatio. Runtimes recommend
	 * different render sizes (Meta's follows the Link app's render resolution), so one value doesn't suit all.
	 */
	float SupersampleRatioFor(const std::string& runtimeName) const;
	bool Haptics() const { return haptics; }
	bool AdmitUnknownProps() const { return admitUnknownProps; }
	inline bool LogGetTrackedProperty() const { return logGetTrackedProperty; }
	inline bool StopOnSoftAbort() const { return stopOnSoftAbort; }
	inline bool DX10Mode() const { return dx10Mode; }
	inline bool EnableHiddenMeshFix() const { return enableHiddenMeshFix; }
	inline bool InvertUsingShaders() const { return invertUsingShaders; }
	inline bool InitUsingVulkan() const { return initUsingVulkan; }
	float HiddenMeshVerticalScale() const { return hiddenMeshVerticalScale; }
	inline bool LogAllOpenVRCalls() const { return logAllOpenVRCalls; }

	/**
	 * Which head pose the projection layer is submitted with (port of MajesticZeta's F1-25-wobble-fix, GPLv3).
	 * 0: the pose located for this frame in WaitGetPoses (stock OpenComposite).
	 * 1: the pose located for the previous frame. F1 25 renders each frame from the previous frame's pose but
	 *    labels it with the newest, so the runtime reprojected it as one frame fresher than it is: the cockpit
	 *    briefly followed the head, then snapped back. Default 1 here (this build is for F1 25).
	 */
	int PoseSyncMode() const { return (int)poseSyncMode; }

private:
	static int ini_handler(
	    void* user, const char* section,
	    const char* name, const char* value,
	    int lineno);

	bool renderCustomHands = true;
	vr::HmdColor_t handColour = vr::HmdColor_t{ 0.3f, 0.3f, 0.3f, 1 };
	float supersampleRatio = 1.0f;
	bool haptics = true;
	bool admitUnknownProps = false;
	bool logGetTrackedProperty = false;
	bool stopOnSoftAbort = false;
	bool dx10Mode = false;
	bool enableHiddenMeshFix = true;
	bool invertUsingShaders = false;
	bool initUsingVulkan = false;
	float hiddenMeshVerticalScale = 1.0f;
	bool logAllOpenVRCalls = false;
	float poseSyncMode = 1.0f;
	std::vector<std::pair<std::string, float>> runtimeSupersampleRatios; // (lowercase section name, ratio)
};

extern Config oovr_global_configuration;
