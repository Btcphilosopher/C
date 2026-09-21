1. Core Surface types
// include/SurfaceTypes.hpp

#pragma once

#include <cstdint>

namespace surface
{

enum class PowerMode
{
    BatterySaver,
    Balanced,
    Performance,
    ExtremePerformance
};

enum class PowerSource
{
    Battery,
    AC,
    Unknown
};

struct BatteryState
{
    int percentage{0};

    bool charging{false};
    bool fullyCharged{false};

    double voltage{0.0};
    double currentAmps{0.0};

    double temperatureC{0.0};

    PowerSource source{PowerSource::Unknown};
};

struct ThermalState
{
    double cpuTemperatureC{0.0};
    double gpuTemperatureC{0.0};
    double batteryTemperatureC{0.0};

    double cpuTemperatureLimitC{95.0};
    double gpuTemperatureLimitC{95.0};

    bool thermalPressure{false};
    bool criticalTemperature{false};
};

struct SystemLoad
{
    double cpuUsagePercent{0.0};
    double gpuUsagePercent{0.0};
    double npuUsagePercent{0.0};

    double memoryUsagePercent{0.0};

    std::uint32_t activeProcesses{0};
};

struct DisplayState
{
    unsigned refreshRateHz{60};
    unsigned brightnessPercent{50};

    bool hdrEnabled{false};
};

struct SurfaceTelemetry
{
    BatteryState battery;
    ThermalState thermal;
    SystemLoad load;
    DisplayState display;
};

struct PowerLimits
{
    double cpuWatts{15.0};
    double gpuWatts{10.0};
    double npuWatts{5.0};

    unsigned refreshRateHz{60};
};

struct PowerPolicy
{
    PowerMode mode{PowerMode::Balanced};

    PowerLimits limits;

    bool reduceDisplayRefreshRate{false};
    bool reduceBrightness{false};

    bool enableAggressiveThermalControl{false};

    double targetCpuWatts{15.0};
    double targetGpuWatts{10.0};
    double targetNpuWatts{5.0};
};

}
2. Battery manager

This uses the Windows power-status API for the basic system battery state.

// include/BatteryManager.hpp

#pragma once

#include "SurfaceTypes.hpp"

namespace surface
{

class BatteryManager
{
public:

    BatteryState readState() const;

private:

    double readBatteryTemperature() const;
    double readVoltage() const;
    double readCurrent() const;
};

}
// src/BatteryManager.cpp

#include "BatteryManager.hpp"

#include <windows.h>
#include <algorithm>

namespace surface
{

BatteryState BatteryManager::readState() const
{
    BatteryState state;

    SYSTEM_POWER_STATUS status{};

    if (!GetSystemPowerStatus(&status))
    {
        state.source = PowerSource::Unknown;
        return state;
    }

    state.percentage =
        status.BatteryLifePercent == 255
            ? 0
            : static_cast<int>(status.BatteryLifePercent);

    state.charging =
        (status.ACLineStatus == 1) &&
        !(status.BatteryFlag & 8);

    state.fullyCharged =
        (status.BatteryFlag & 8) != 0;

    if (status.ACLineStatus == 1)
    {
        state.source = PowerSource::AC;
    }
    else if (status.ACLineStatus == 0)
    {
        state.source = PowerSource::Battery;
    }
    else
    {
        state.source = PowerSource::Unknown;
    }

    state.temperatureC = readBatteryTemperature();
    state.voltage = readVoltage();
    state.currentAmps = readCurrent();

    return state;
}

double BatteryManager::readBatteryTemperature() const
{
    /*
        Real Surface battery temperature should normally come
        from an appropriate Windows device/driver interface.

        Returning zero here means "not available".
    */

    return 0.0;
}

double BatteryManager::readVoltage() const
{
    return 0.0;
}

double BatteryManager::readCurrent() const
{
    return 0.0;
}

}

The important distinction here is that we don't invent a hardware interface. The Windows battery percentage/AC state is accessible through documented APIs; detailed Surface-specific telemetry would require the appropriate hardware/driver path.

3. Thermal manager
// include/ThermalManager.hpp

#pragma once

#include "SurfaceTypes.hpp"

namespace surface
{

class ThermalManager
{
public:

    ThermalState readState() const;

    bool isThermallyConstrained(
        const ThermalState& state) const;

private:

    double readCpuTemperature() const;
    double readGpuTemperature() const;
};

}
// src/ThermalManager.cpp

#include "ThermalManager.hpp"

#include <algorithm>

namespace surface
{

ThermalState ThermalManager::readState() const
{
    ThermalState state;

    state.cpuTemperatureC =
        readCpuTemperature();

    state.gpuTemperatureC =
        readGpuTemperature();

    state.thermalPressure =
        isThermallyConstrained(state);

    state.criticalTemperature =
        state.cpuTemperatureC >=
            state.cpuTemperatureLimitC ||
        state.gpuTemperatureC >=
            state.gpuTemperatureLimitC;

    return state;
}

bool ThermalManager::isThermallyConstrained(
    const ThermalState& state) const
{
    constexpr double warningTemperature = 85.0;

    return
        state.cpuTemperatureC >= warningTemperature ||
        state.gpuTemperatureC >= warningTemperature;
}

double ThermalManager::readCpuTemperature() const
{
    /*
        Placeholder for actual Windows thermal telemetry.

        Depending on hardware and supported interfaces,
        temperature data can come through platform/device
        interfaces rather than a generic C++ API.
    */

    return 0.0;
}

double ThermalManager::readGpuTemperature() const
{
    return 0.0;
}

}
4. System telemetry

Now we combine the various components.

// include/SystemTelemetry.hpp

#pragma once

#include "SurfaceTypes.hpp"
#include "BatteryManager.hpp"
#include "ThermalManager.hpp"

namespace surface
{

class SystemTelemetry
{
public:

    SurfaceTelemetry collect();

private:

    BatteryManager batteryManager;
    ThermalManager thermalManager;
};

}
// src/SystemTelemetry.cpp

#include "SystemTelemetry.hpp"

#include <windows.h>

namespace surface
{

SurfaceTelemetry SystemTelemetry::collect()
{
    SurfaceTelemetry telemetry;

    telemetry.battery =
        batteryManager.readState();

    telemetry.thermal =
        thermalManager.readState();

    /*
        CPU/GPU/NPU telemetry would normally be obtained
        through Windows performance counters, ETW,
        device-specific interfaces, or vendor APIs.
    */

    telemetry.load.cpuUsagePercent = 0.0;
    telemetry.load.gpuUsagePercent = 0.0;
    telemetry.load.npuUsagePercent = 0.0;

    telemetry.load.memoryUsagePercent = 0.0;

    telemetry.display.refreshRateHz = 60;
    telemetry.display.brightnessPercent = 50;

    return telemetry;
}

}
5. The actual power-policy engine

This is the heart of the system.

// include/PowerPolicy.hpp

#pragma once

#include "SurfaceTypes.hpp"

namespace surface
{

class PowerPolicyEngine
{
public:

    PowerPolicy calculate(
        const SurfaceTelemetry& telemetry) const;

private:

    PowerMode determineMode(
        const SurfaceTelemetry& telemetry) const;

    PowerLimits calculateLimits(
        PowerMode mode,
        const SurfaceTelemetry& telemetry) const;

    void applyThermalConstraints(
        PowerLimits& limits,
        const ThermalState& thermal) const;

    void applyBatteryConstraints(
        PowerLimits& limits,
        const BatteryState& battery) const;
};

}
// src/PowerPolicy.cpp

#include "PowerPolicy.hpp"

#include <algorithm>

namespace surface
{

PowerPolicy PowerPolicyEngine::calculate(
    const SurfaceTelemetry& telemetry) const
{
    PowerPolicy policy;

    policy.mode =
        determineMode(telemetry);

    policy.limits =
        calculateLimits(
            policy.mode,
            telemetry);

    policy.targetCpuWatts =
        policy.limits.cpuWatts;

    policy.targetGpuWatts =
        policy.limits.gpuWatts;

    policy.targetNpuWatts =
        policy.limits.npuWatts;

    policy.reduceDisplayRefreshRate =
        policy.limits.refreshRateHz < 120;

    policy.reduceBrightness =
        telemetry.battery.percentage < 20;

    policy.enableAggressiveThermalControl =
        telemetry.thermal.thermalPressure;

    return policy;
}

PowerMode PowerPolicyEngine::determineMode(
    const SurfaceTelemetry& telemetry) const
{
    const auto& battery = telemetry.battery;
    const auto& load = telemetry.load;
    const auto& thermal = telemetry.thermal;

    if (thermal.criticalTemperature)
    {
        return PowerMode::BatterySaver;
    }

    if (battery.source == PowerSource::Battery)
    {
        if (battery.percentage <= 15)
        {
            return PowerMode::BatterySaver;
        }

        if (battery.percentage <= 40)
        {
            return PowerMode::Balanced;
        }
    }

    const bool highPerformanceWorkload =
        load.cpuUsagePercent > 80.0 ||
        load.gpuUsagePercent > 80.0 ||
        load.npuUsagePercent > 80.0;

    if (battery.source == PowerSource::AC &&
        highPerformanceWorkload &&
        !thermal.thermalPressure)
    {
        return PowerMode::Performance;
    }

    return PowerMode::Balanced;
}

PowerLimits PowerPolicyEngine::calculateLimits(
    PowerMode mode,
    const SurfaceTelemetry& telemetry) const
{
    PowerLimits limits;

    switch (mode)
    {
        case PowerMode::BatterySaver:

            limits.cpuWatts = 8.0;
            limits.gpuWatts = 4.0;
            limits.npuWatts = 2.0;
            limits.refreshRateHz = 60;

            break;

        case PowerMode::Balanced:

            limits.cpuWatts = 15.0;
            limits.gpuWatts = 8.0;
            limits.npuWatts = 4.0;
            limits.refreshRateHz = 60;

            break;

        case PowerMode::Performance:

            limits.cpuWatts = 25.0;
            limits.gpuWatts = 15.0;
            limits.npuWatts = 8.0;
            limits.refreshRateHz = 120;

            break;

        case PowerMode::ExtremePerformance:

            limits.cpuWatts = 35.0;
            limits.gpuWatts = 25.0;
            limits.npuWatts = 15.0;
            limits.refreshRateHz = 120;

            break;
    }

    applyBatteryConstraints(
        limits,
        telemetry.battery);

    applyThermalConstraints(
        limits,
        telemetry.thermal);

    return limits;
}

void PowerPolicyEngine::applyThermalConstraints(
    PowerLimits& limits,
    const ThermalState& thermal) const
{
    if (thermal.cpuTemperatureC >= 90.0)
    {
        limits.cpuWatts *= 0.70;
    }

    if (thermal.gpuTemperatureC >= 90.0)
    {
        limits.gpuWatts *= 0.70;
    }

    if (thermal.cpuTemperatureC >= 95.0)
    {
        limits.cpuWatts *= 0.50;
    }

    if (thermal.gpuTemperatureC >= 95.0)
    {
        limits.gpuWatts *= 0.50;
    }

    limits.cpuWatts =
        std::max(3.0, limits.cpuWatts);

    limits.gpuWatts =
        std::max(1.0, limits.gpuWatts);
}

void PowerPolicyEngine::applyBatteryConstraints(
    PowerLimits& limits,
    const BatteryState& battery) const
{
    if (battery.source != PowerSource::Battery)
    {
        return;
    }

    if (battery.percentage <= 20)
    {
        limits.cpuWatts *= 0.70;
        limits.gpuWatts *= 0.60;
        limits.npuWatts *= 0.70;

        limits.refreshRateHz = 60;
    }

    if (battery.percentage <= 10)
    {
        limits.cpuWatts *= 0.70;
        limits.gpuWatts *= 0.60;
        limits.npuWatts *= 0.60;
    }
}

}
6. Surface Power Manager

Now we create the service that actually runs the policy loop.

// include/SurfacePowerManager.hpp

#pragma once

#include "SurfaceTypes.hpp"
#include "SystemTelemetry.hpp"
#include "PowerPolicy.hpp"

#include <atomic>

namespace surface
{

class SurfacePowerManager
{
public:

    SurfacePowerManager();

    void start();
    void stop();

    bool running() const;

private:

    void controlLoop();

    void applyPolicy(
        const PowerPolicy& policy);

    SystemTelemetry telemetry;
    PowerPolicyEngine policyEngine;

    std::atomic<bool> runningFlag{false};
};

}
// src/SurfacePowerManager.cpp

#include "SurfacePowerManager.hpp"

#include <iostream>
#include <thread>
#include <chrono>

namespace surface
{

SurfacePowerManager::SurfacePowerManager()
{
}

void SurfacePowerManager::start()
{
    if (runningFlag.exchange(true))
    {
        return;
    }

    std::thread(
        &SurfacePowerManager::controlLoop,
        this
    ).detach();
}

void SurfacePowerManager::stop()
{
    runningFlag = false;
}

bool SurfacePowerManager::running() const
{
    return runningFlag;
}

void SurfacePowerManager::controlLoop()
{
    while (runningFlag)
    {
        const SurfaceTelemetry state =
            telemetry.collect();

        const PowerPolicy policy =
            policyEngine.calculate(state);

        applyPolicy(policy);

        std::this_thread::sleep_for(
            std::chrono::seconds(1));
    }
}

void SurfacePowerManager::applyPolicy(
    const PowerPolicy& policy)
{
    /*
        IMPORTANT:

        This is where platform-specific policy
        application would occur.

        A production implementation would use
        appropriate Windows power-management APIs
        and supported device interfaces rather than
        attempting to write arbitrary hardware registers.
    */

    std::cout
        << "Power policy: ";

    switch (policy.mode)
    {
        case PowerMode::BatterySaver:
            std::cout << "Battery Saver";
            break;

        case PowerMode::Balanced:
            std::cout << "Balanced";
            break;

        case PowerMode::Performance:
            std::cout << "Performance";
            break;

        case PowerMode::ExtremePerformance:
            std::cout << "Extreme Performance";
            break;
    }

    std::cout << "\n";

    std::cout
        << "CPU target: "
        << policy.targetCpuWatts
        << " W\n";

    std::cout
        << "GPU target: "
        << policy.targetGpuWatts
        << " W\n";

    std::cout
        << "NPU target: "
        << policy.targetNpuWatts
        << " W\n";

    std::cout
        << "Display refresh target: "
        << policy.limits.refreshRateHz
        << " Hz\n";

    std::cout << "----------------------\n";
}

}
7. Main program
// src/main.cpp

#include "SurfacePowerManager.hpp"

#include <iostream>
#include <thread>
#include <chrono>

int main()
{
    surface::SurfacePowerManager manager;

    std::cout
        << "Microsoft Surface Adaptive Power Manager\n";

    std::cout
        << "-----------------------------------------\n";

    manager.start();

    std::cout
        << "Power management engine running.\n";

    std::cout
        << "Press ENTER to stop.\n";

    std::cin.get();

    manager.stop();

    std::cout
        << "Power management engine stopped.\n";

    return 0;
}
8. CMake
cmake_minimum_required(VERSION 3.20)

project(
    SurfacePowerManager
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(
    SurfacePowerManager

    src/main.cpp
    src/BatteryManager.cpp
    src/ThermalManager.cpp
    src/SystemTelemetry.cpp
    src/PowerPolicy.cpp
    src/SurfacePowerManager.cpp
)

target_include_directories(
    SurfacePowerManager
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if (WIN32)

    target_compile_definitions(
        SurfacePowerManager
        PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )

endif()








Project structure
SurfaceThermalEngine/
├── CMakeLists.txt
├── include/
│   ├── ThermalTypes.hpp
│   ├── ThermalSensor.hpp
│   ├── ThermalModel.hpp
│   ├── ThermalPolicy.hpp
│   └── SurfaceThermalEngine.hpp
├── src/
│   ├── ThermalSensor.cpp
│   ├── ThermalModel.cpp
│   ├── ThermalPolicy.cpp
│   ├── SurfaceThermalEngine.cpp
│   └── main.cpp
└── tests/
    └── ThermalPolicyTests.cpp
include/ThermalTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>

namespace surface::thermal {

enum class ThermalZone {
    CPU,
    GPU,
    NPU,
    Battery,
    Chassis,
    Unknown
};

enum class ThermalState {
    Cool,
    Normal,
    Warm,
    Hot,
    Critical
};

enum class CoolingAction {
    None,
    ReduceCPU,
    ReduceGPU,
    ReduceNPU,
    ReduceDisplay,
    IncreaseCooling,
    EmergencyThrottle
};

struct TemperatureReading {
    ThermalZone zone{ThermalZone::Unknown};
    double temperatureC{0.0};
    bool valid{false};

    std::chrono::steady_clock::time_point timestamp{
        std::chrono::steady_clock::now()
    };
};

struct ThermalLimits {
    double warmC{70.0};
    double hotC{85.0};
    double criticalC{95.0};
    double emergencyC{100.0};
};

struct ThermalSnapshot {
    TemperatureReading cpu;
    TemperatureReading gpu;
    TemperatureReading npu;
    TemperatureReading battery;
    TemperatureReading chassis;

    ThermalState cpuState{ThermalState::Cool};
    ThermalState gpuState{ThermalState::Cool};
    ThermalState npuState{ThermalState::Cool};
    ThermalState batteryState{ThermalState::Cool};

    double predictedCpuC{0.0};
    double predictedGpuC{0.0};

    bool thermalPressure{false};
    bool critical{false};
};

struct ThermalPolicy {
    double cpuPerformanceFactor{1.0};
    double gpuPerformanceFactor{1.0};
    double npuPerformanceFactor{1.0};

    unsigned targetRefreshRateHz{120};

    CoolingAction primaryAction{CoolingAction::None};

    bool reduceDisplayPower{false};
    bool aggressiveCooling{false};
    bool emergencyProtection{false};
};

} // namespace surface::thermal
include/ThermalSensor.hpp
#pragma once

#include "ThermalTypes.hpp"

namespace surface::thermal {

class IThermalSensor {
public:
    virtual ~IThermalSensor() = default;

    virtual TemperatureReading read(
        ThermalZone zone) = 0;
};

class WindowsThermalSensor final : public IThermalSensor {
public:
    TemperatureReading read(
        ThermalZone zone) override;

private:
    TemperatureReading unavailable(
        ThermalZone zone) const;
};

} // namespace surface::thermal
src/ThermalSensor.cpp
#include "ThermalSensor.hpp"

#include <windows.h>

namespace surface::thermal {

TemperatureReading WindowsThermalSensor::read(
    ThermalZone zone)
{
    /*
        Surface thermal sensors are hardware/firmware dependent.

        Do not invent undocumented sensor addresses.

        A production implementation can connect this interface
        to supported Windows/device-driver telemetry.

        Returning invalid is safer than fabricating a temperature.
    */

    return unavailable(zone);
}

TemperatureReading WindowsThermalSensor::unavailable(
    ThermalZone zone) const
{
    TemperatureReading reading;

    reading.zone = zone;
    reading.temperatureC = 0.0;
    reading.valid = false;

    return reading;
}

} // namespace surface::thermal
include/ThermalModel.hpp
#pragma once

#include "ThermalTypes.hpp"

namespace surface::thermal {

class ThermalModel {
public:

    explicit ThermalModel(
        ThermalLimits limits = {});

    ThermalState classify(
        double temperatureC) const;

    double predict(
        double currentTemperatureC,
        double previousTemperatureC,
        double intervalSeconds,
        double horizonSeconds) const;

    bool isCritical(
        double temperatureC) const;

    bool isEmergency(
        double temperatureC) const;

private:

    ThermalLimits limits_;
};

} // namespace surface::thermal
src/ThermalModel.cpp
#include "ThermalModel.hpp"

#include <algorithm>
#include <cmath>

namespace surface::thermal {

ThermalModel::ThermalModel(
    ThermalLimits limits)
    : limits_(limits)
{
}

ThermalState ThermalModel::classify(
    double temperatureC) const
{
    if (temperatureC >= limits_.emergencyC)
        return ThermalState::Critical;

    if (temperatureC >= limits_.criticalC)
        return ThermalState::Critical;

    if (temperatureC >= limits_.hotC)
        return ThermalState::Hot;

    if (temperatureC >= limits_.warmC)
        return ThermalState::Warm;

    if (temperatureC >= 45.0)
        return ThermalState::Normal;

    return ThermalState::Cool;
}

bool ThermalModel::isCritical(
    double temperatureC) const
{
    return temperatureC >= limits_.criticalC;
}

bool ThermalModel::isEmergency(
    double temperatureC) const
{
    return temperatureC >= limits_.emergencyC;
}

double ThermalModel::predict(
    double currentTemperatureC,
    double previousTemperatureC,
    double intervalSeconds,
    double horizonSeconds) const
{
    if (intervalSeconds <= 0.0)
        return currentTemperatureC;

    /*
        First-order prediction.

        rate = °C / second

        This is deliberately conservative and simple.
        A production model could use a calibrated thermal RC model,
        historical telemetry and workload information.
    */

    const double rate =
        (currentTemperatureC - previousTemperatureC)
        / intervalSeconds;

    const double predicted =
        currentTemperatureC +
        rate * horizonSeconds;

    return std::clamp(
        predicted,
        -20.0,
        150.0
    );
}

} // namespace surface::thermal
include/ThermalPolicy.hpp
#pragma once

#include "ThermalTypes.hpp"
#include "ThermalModel.hpp"

namespace surface::thermal {

class ThermalPolicyEngine {
public:

    explicit ThermalPolicyEngine(
        ThermalLimits limits = {});

    ThermalPolicy calculate(
        const ThermalSnapshot& snapshot) const;

private:

    ThermalModel model_;

    ThermalPolicy normalPolicy() const;
    ThermalPolicy warmPolicy() const;
    ThermalPolicy hotPolicy() const;
    ThermalPolicy criticalPolicy() const;
    ThermalPolicy emergencyPolicy() const;

    void applyBatteryProtection(
        ThermalPolicy& policy,
        double batteryTemperature) const;
};

} // namespace surface::thermal
src/ThermalPolicy.cpp
#include "ThermalPolicy.hpp"

#include <algorithm>

namespace surface::thermal {

ThermalPolicyEngine::ThermalPolicyEngine(
    ThermalLimits limits)
    : model_(limits)
{
}

ThermalPolicy
ThermalPolicyEngine::calculate(
    const ThermalSnapshot& snapshot) const
{
    ThermalPolicy policy = normalPolicy();

    const double cpu =
        snapshot.cpu.valid
            ? snapshot.cpu.temperatureC
            : 0.0;

    const double gpu =
        snapshot.gpu.valid
            ? snapshot.gpu.temperatureC
            : 0.0;

    const double npu =
        snapshot.npu.valid
            ? snapshot.npu.temperatureC
            : 0.0;

    const double battery =
        snapshot.battery.valid
            ? snapshot.battery.temperatureC
            : 0.0;

    const double maximumTemperature =
        std::max({
            cpu,
            gpu,
            npu
        });

    if (model_.isEmergency(maximumTemperature))
    {
        policy = emergencyPolicy();
    }
    else if (model_.isCritical(maximumTemperature))
    {
        policy = criticalPolicy();
    }
    else if (
        model_.classify(maximumTemperature)
        == ThermalState::Hot)
    {
        policy = hotPolicy();
    }
    else if (
        model_.classify(maximumTemperature)
        == ThermalState::Warm)
    {
        policy = warmPolicy();
    }

    /*
        CPU-specific protection.
    */

    if (cpu >= 90.0)
    {
        policy.cpuPerformanceFactor =
            std::min(
                policy.cpuPerformanceFactor,
                0.65
            );
    }

    /*
        GPU-specific protection.
    */

    if (gpu >= 90.0)
    {
        policy.gpuPerformanceFactor =
            std::min(
                policy.gpuPerformanceFactor,
                0.65
            );
    }

    /*
        NPU-specific protection.
    */

    if (npu >= 90.0)
    {
        policy.npuPerformanceFactor =
            std::min(
                policy.npuPerformanceFactor,
                0.70
            );
    }

    /*
        Battery thermal protection.
    */

    applyBatteryProtection(
        policy,
        battery
    );

    return policy;
}

ThermalPolicy
ThermalPolicyEngine::normalPolicy() const
{
    ThermalPolicy policy;

    policy.cpuPerformanceFactor = 1.0;
    policy.gpuPerformanceFactor = 1.0;
    policy.npuPerformanceFactor = 1.0;

    policy.targetRefreshRateHz = 120;

    policy.primaryAction =
        CoolingAction::None;

    return policy;
}

ThermalPolicy
ThermalPolicyEngine::warmPolicy() const
{
    ThermalPolicy policy;

    policy.cpuPerformanceFactor = 0.90;
    policy.gpuPerformanceFactor = 0.90;
    policy.npuPerformanceFactor = 0.95;

    policy.targetRefreshRateHz = 120;

    policy.primaryAction =
        CoolingAction::IncreaseCooling;

    policy.aggressiveCooling = true;

    return policy;
}

ThermalPolicy
ThermalPolicyEngine::hotPolicy() const
{
    ThermalPolicy policy;

    policy.cpuPerformanceFactor = 0.75;
    policy.gpuPerformanceFactor = 0.70;
    policy.npuPerformanceFactor = 0.80;

    policy.targetRefreshRateHz = 90;

    policy.primaryAction =
        CoolingAction::ReduceCPU;

    policy.reduceDisplayPower = true;
    policy.aggressiveCooling = true;

    return policy;
}

ThermalPolicy
ThermalPolicyEngine::criticalPolicy() const
{
    ThermalPolicy policy;

    policy.cpuPerformanceFactor = 0.50;
    policy.gpuPerformanceFactor = 0.45;
    policy.npuPerformanceFactor = 0.50;

    policy.targetRefreshRateHz = 60;

    policy.primaryAction =
        CoolingAction::EmergencyThrottle;

    policy.reduceDisplayPower = true;
    policy.aggressiveCooling = true;
    policy.emergencyProtection = true;

    return policy;
}

ThermalPolicy
ThermalPolicyEngine::emergencyPolicy() const
{
    ThermalPolicy policy;

    policy.cpuPerformanceFactor = 0.25;
    policy.gpuPerformanceFactor = 0.20;
    policy.npuPerformanceFactor = 0.25;

    policy.targetRefreshRateHz = 60;

    policy.primaryAction =
        CoolingAction::EmergencyThrottle;

    policy.reduceDisplayPower = true;
    policy.aggressiveCooling = true;
    policy.emergencyProtection = true;

    return policy;
}

void ThermalPolicyEngine::applyBatteryProtection(
    ThermalPolicy& policy,
    double batteryTemperature) const
{
    if (batteryTemperature <= 0.0)
        return;

    if (batteryTemperature >= 45.0)
    {
        policy.cpuPerformanceFactor =
            std::min(
                policy.cpuPerformanceFactor,
                0.70
            );

        policy.gpuPerformanceFactor =
            std::min(
                policy.gpuPerformanceFactor,
                0.60
            );

        policy.npuPerformanceFactor =
            std::min(
                policy.npuPerformanceFactor,
                0.70
            );

        policy.reduceDisplayPower = true;
    }

    if (batteryTemperature >= 50.0)
    {
        policy.cpuPerformanceFactor =
            std::min(
                policy.cpuPerformanceFactor,
                0.40
            );

        policy.gpuPerformanceFactor =
            std::min(
                policy.gpuPerformanceFactor,
                0.35
            );

        policy.npuPerformanceFactor =
            std::min(
                policy.npuPerformanceFactor,
                0.40
            );

        policy.primaryAction =
            CoolingAction::EmergencyThrottle;

        policy.emergencyProtection = true;
    }
}

} // namespace surface::thermal
include/SurfaceThermalEngine.hpp
#pragma once

#include "ThermalSensor.hpp"
#include "ThermalModel.hpp"
#include "ThermalPolicy.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace surface::thermal {

class SurfaceThermalEngine {
public:

    SurfaceThermalEngine();

    ~SurfaceThermalEngine();

    SurfaceThermalEngine(
        const SurfaceThermalEngine&) = delete;

    SurfaceThermalEngine& operator=(
        const SurfaceThermalEngine&) = delete;

    void start();

    void stop();

    bool running() const;

    ThermalSnapshot snapshot() const;

    ThermalPolicy policy() const;

private:

    void controlLoop();

    ThermalSnapshot collectSnapshot();

    void applyPolicy(
        const ThermalPolicy& policy);

    std::unique_ptr<IThermalSensor> sensor_;

    ThermalModel model_;
    ThermalPolicyEngine policyEngine_;

    mutable std::mutex mutex_;

    ThermalSnapshot latestSnapshot_;
    ThermalPolicy latestPolicy_;

    std::atomic<bool> running_{false};

    std::thread worker_;

    TemperatureReading previousCpu_;
    TemperatureReading previousGpu_;
};

} // namespace surface::thermal
src/SurfaceThermalEngine.cpp
#include "SurfaceThermalEngine.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace surface::thermal {

SurfaceThermalEngine::SurfaceThermalEngine()
    : sensor_(
        std::make_unique<WindowsThermalSensor>()
      )
{
}

SurfaceThermalEngine::~SurfaceThermalEngine()
{
    stop();
}

void SurfaceThermalEngine::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    worker_ =
        std::thread(
            &SurfaceThermalEngine::controlLoop,
            this
        );
}

void SurfaceThermalEngine::stop()
{
    running_ = false;

    if (worker_.joinable())
    {
        worker_.join();
    }
}

bool SurfaceThermalEngine::running() const
{
    return running_;
}

ThermalSnapshot
SurfaceThermalEngine::snapshot() const
{
    std::lock_guard lock(mutex_);

    return latestSnapshot_;
}

ThermalPolicy
SurfaceThermalEngine::policy() const
{
    std::lock_guard lock(mutex_);

    return latestPolicy_;
}

ThermalSnapshot
SurfaceThermalEngine::collectSnapshot()
{
    ThermalSnapshot snapshot;

    snapshot.cpu =
        sensor_->read(ThermalZone::CPU);

    snapshot.gpu =
        sensor_->read(ThermalZone::GPU);

    snapshot.npu =
        sensor_->read(ThermalZone::NPU);

    snapshot.battery =
        sensor_->read(ThermalZone::Battery);

    snapshot.chassis =
        sensor_->read(ThermalZone::Chassis);

    if (snapshot.cpu.valid)
    {
        snapshot.cpuState =
            model_.classify(
                snapshot.cpu.temperatureC
            );
    }

    if (snapshot.gpu.valid)
    {
        snapshot.gpuState =
            model_.classify(
                snapshot.gpu.temperatureC
            );
    }

    if (snapshot.npu.valid)
    {
        snapshot.npuState =
            model_.classify(
                snapshot.npu.temperatureC
            );
    }

    if (snapshot.battery.valid)
    {
        snapshot.batteryState =
            model_.classify(
                snapshot.battery.temperatureC
            );
    }

    /*
        Predict five seconds ahead.
    */

    if (
        snapshot.cpu.valid &&
        previousCpu_.valid
    )
    {
        const double interval =
            std::chrono::duration<double>(
                snapshot.cpu.timestamp -
                previousCpu_.timestamp
            ).count();

        snapshot.predictedCpuC =
            model_.predict(
                snapshot.cpu.temperatureC,
                previousCpu_.temperatureC,
                interval,
                5.0
            );
    }
    else if (snapshot.cpu.valid)
    {
        snapshot.predictedCpuC =
            snapshot.cpu.temperatureC;
    }

    if (
        snapshot.gpu.valid &&
        previousGpu_.valid
    )
    {
        const double interval =
            std::chrono::duration<double>(
                snapshot.gpu.timestamp -
                previousGpu_.timestamp
            ).count();

        snapshot.predictedGpuC =
            model_.predict(
                snapshot.gpu.temperatureC,
                previousGpu_.temperatureC,
                interval,
                5.0
            );
    }
    else if (snapshot.gpu.valid)
    {
        snapshot.predictedGpuC =
            snapshot.gpu.temperatureC;
    }

    previousCpu_ = snapshot.cpu;
    previousGpu_ = snapshot.gpu;

    const double maximum =
        std::max({
            snapshot.cpu.valid
                ? snapshot.cpu.temperatureC
                : 0.0,

            snapshot.gpu.valid
                ? snapshot.gpu.temperatureC
                : 0.0,

            snapshot.npu.valid
                ? snapshot.npu.temperatureC
                : 0.0
        });

    snapshot.thermalPressure =
        maximum >= 70.0;

    snapshot.critical =
        maximum >= 95.0;

    return snapshot;
}

void SurfaceThermalEngine::controlLoop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        ThermalSnapshot current =
            collectSnapshot();

        ThermalPolicy calculated =
            policyEngine_.calculate(
                current
            );

        {
            std::lock_guard lock(mutex_);

            latestSnapshot_ =
                current;

            latestPolicy_ =
                calculated;
        }

        applyPolicy(calculated);

        std::this_thread::sleep_for(
            1s
        );
    }
}

void SurfaceThermalEngine::applyPolicy(
    const ThermalPolicy& policy)
{
    /*
        Policy application is intentionally abstract.

        Actual implementation should connect this layer
        to supported Windows power-management and
        device-driver interfaces.

        Do not attempt arbitrary MSR/register writes.
    */

    std::cout
        << "[THERMAL] "
        << "CPU="
        << policy.cpuPerformanceFactor * 100.0
        << "% GPU="
        << policy.gpuPerformanceFactor * 100.0
        << "% NPU="
        << policy.npuPerformanceFactor * 100.0
        << "% Refresh="
        << policy.targetRefreshRateHz
        << "Hz";

    if (policy.aggressiveCooling)
    {
        std::cout
            << " COOLING=AGGRESSIVE";
    }

    if (policy.emergencyProtection)
    {
        std::cout
            << " PROTECTION=EMERGENCY";
    }

    std::cout << '\n';
}

} // namespace surface::thermal
src/main.cpp
#include "SurfaceThermalEngine.hpp"

#include <iostream>

int main()
{
    surface::thermal::SurfaceThermalEngine engine;

    std::cout
        << "========================================\n"
        << " Surface Thermal Management Engine\n"
        << " C++20 / Windows\n"
        << "========================================\n\n";

    engine.start();

    std::cout
        << "Thermal engine started.\n"
        << "Press ENTER to stop.\n";

    std::cin.get();

    engine.stop();

    std::cout
        << "Thermal engine stopped.\n";

    return 0;
}
tests/ThermalPolicyTests.cpp
#include "ThermalPolicy.hpp"

#include <cassert>
#include <iostream>

using namespace surface::thermal;

int main()
{
    ThermalPolicyEngine engine;

    {
        ThermalSnapshot snapshot;

        snapshot.cpu.valid = true;
        snapshot.cpu.temperatureC = 50.0;

        snapshot.gpu.valid = true;
        snapshot.gpu.temperatureC = 50.0;

        ThermalPolicy policy =
            engine.calculate(snapshot);

        assert(
            policy.cpuPerformanceFactor == 1.0
        );

        assert(
            !policy.emergencyProtection
        );
    }

    {
        ThermalSnapshot snapshot;

        snapshot.cpu.valid = true;
        snapshot.cpu.temperatureC = 88.0;

        ThermalPolicy policy =
            engine.calculate(snapshot);

        assert(
            policy.cpuPerformanceFactor < 1.0
        );

        assert(
            policy.aggressiveCooling
        );
    }

    {
        ThermalSnapshot snapshot;

        snapshot.cpu.valid = true;
        snapshot.cpu.temperatureC = 97.0;

        ThermalPolicy policy =
            engine.calculate(snapshot);

        assert(
            policy.emergencyProtection
        );

        assert(
            policy.cpuPerformanceFactor <= 0.50
        );

        assert(
            policy.targetRefreshRateHz == 60
        );
    }

    std::cout
        << "All thermal policy tests passed.\n";

    return 0;
}
CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceThermalEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(
    SurfaceThermalEngine

    src/main.cpp
    src/ThermalSensor.cpp
    src/ThermalModel.cpp
    src/ThermalPolicy.cpp
    src/SurfaceThermalEngine.cpp
)

target_include_directories(
    SurfaceThermalEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceThermalEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    ThermalPolicyTests

    tests/ThermalPolicyTests.cpp
    src/ThermalPolicy.cpp
    src/ThermalModel.cpp
)

target_include_directories(
    ThermalPolicyTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME ThermalPolicyTests
    COMMAND ThermalPolicyTests
)




Project structure
SurfacePenEngine/
├── CMakeLists.txt
├── include/
│   ├── PenTypes.hpp
│   ├── PenFilter.hpp
│   ├── PenPredictor.hpp
│   ├── PalmRejection.hpp
│   ├── PenEngine.hpp
│   └── WindowsPenInput.hpp
├── src/
│   ├── PenFilter.cpp
│   ├── PenPredictor.cpp
│   ├── PalmRejection.cpp
│   ├── PenEngine.cpp
│   ├── WindowsPenInput.cpp
│   └── main.cpp
└── tests/
    └── PenTests.cpp
include/PenTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>

namespace surface::pen {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct PenPoint
{
    double x{0.0};
    double y{0.0};

    double pressure{0.0};

    double tiltX{0.0};
    double tiltY{0.0};

    double rotation{0.0};

    TimePoint timestamp{Clock::now()};
};

struct PenVelocity
{
    double x{0.0};
    double y{0.0};
};

struct PenState
{
    bool inRange{false};
    bool tipDown{false};
    bool barrelButton{false};
    bool eraser{false};

    PenPoint point;
    PenVelocity velocity;
};

struct PredictedPoint
{
    double x{0.0};
    double y{0.0};

    double pressure{0.0};

    double tiltX{0.0};
    double tiltY{0.0};

    TimePoint timestamp{Clock::now()};
};

struct PalmContact
{
    double x{0.0};
    double y{0.0};

    double width{0.0};
    double height{0.0};

    bool active{false};
};

struct PenConfiguration
{
    double smoothingFactor{0.35};

    double predictionMilliseconds{8.0};

    double palmWidthThreshold{35.0};
    double palmHeightThreshold{35.0};

    double minimumPressure{0.0};
    double maximumPressure{1.0};
};

}
include/WindowsPenInput.hpp
#pragma once

#include "PenTypes.hpp"

#include <functional>
#include <windows.h>

namespace surface::pen {

class WindowsPenInput
{
public:

    using PenCallback =
        std::function<void(const PenState&)>;

    WindowsPenInput() = default;

    void setCallback(
        PenCallback callback);

    void processPointerMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

private:

    PenCallback callback_;

    PenState decodePointer(
        UINT message,
        WPARAM wParam,
        LPARAM lParam) const;

    static double normalizePressure(
        UINT pressure);

};

}
src/WindowsPenInput.cpp
#include "WindowsPenInput.hpp"

#include <windowsx.h>
#include <algorithm>

namespace surface::pen {

void WindowsPenInput::setCallback(
    PenCallback callback)
{
    callback_ = std::move(callback);
}

void WindowsPenInput::processPointerMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (!callback_)
        return;

    PenState state =
        decodePointer(
            message,
            wParam,
            lParam
        );

    callback_(state);
}

PenState WindowsPenInput::decodePointer(
    UINT message,
    WPARAM wParam,
    LPARAM lParam) const
{
    PenState state;

    const UINT32 pointerId =
        GET_POINTERID_WPARAM(wParam);

    POINTER_INFO pointerInfo{};

    if (!GetPointerInfo(
            pointerId,
            &pointerInfo))
    {
        return state;
    }

    state.inRange = true;

    state.tipDown =
        (pointerInfo.pointerFlags &
         POINTER_FLAG_INCONTACT) != 0;

    state.barrelButton =
        (pointerInfo.pointerFlags &
         POINTER_FLAG_FIRSTBUTTON) != 0;

    state.point.x =
        static_cast<double>(
            pointerInfo.ptPixelLocation.x
        );

    state.point.y =
        static_cast<double>(
            pointerInfo.ptPixelLocation.y
        );

    state.point.timestamp =
        Clock::now();

    /*
        Pen-specific information is available through
        POINTER_PEN_INFO where supported.
    */

    if (pointerInfo.pointerType ==
        PT_PEN)
    {
        POINTER_PEN_INFO penInfo{};

        if (GetPointerPenInfo(
                pointerId,
                &penInfo))
        {
            state.point.pressure =
                normalizePressure(
                    penInfo.pressure
                );

            state.point.rotation =
                static_cast<double>(
                    penInfo.rotation
                );

            state.point.tiltX =
                static_cast<double>(
                    penInfo.tiltX
                );

            state.point.tiltY =
                static_cast<double>(
                    penInfo.tiltY
                );
        }
    }

    return state;
}

double WindowsPenInput::normalizePressure(
    UINT pressure)
{
    constexpr double maxPressure =
        1024.0;

    return std::clamp(
        static_cast<double>(pressure)
            / maxPressure,
        0.0,
        1.0
    );
}

}
include/PenFilter.hpp
#pragma once

#include "PenTypes.hpp"

namespace surface::pen {

class PenFilter
{
public:

    explicit PenFilter(
        double smoothingFactor = 0.35);

    PenPoint filter(
        const PenPoint& input);

    void reset();

private:

    double smoothingFactor_;

    bool initialized_{false};

    PenPoint previous_;
};

}
src/PenFilter.cpp
#include "PenFilter.hpp"

#include <algorithm>

namespace surface::pen {

PenFilter::PenFilter(
    double smoothingFactor)
    : smoothingFactor_(
        std::clamp(
            smoothingFactor,
            0.0,
            1.0))
{
}

PenPoint PenFilter::filter(
    const PenPoint& input)
{
    if (!initialized_)
    {
        previous_ = input;
        initialized_ = true;

        return input;
    }

    const double alpha =
        smoothingFactor_;

    PenPoint output = input;

    output.x =
        previous_.x * (1.0 - alpha)
        + input.x * alpha;

    output.y =
        previous_.y * (1.0 - alpha)
        + input.y * alpha;

    output.pressure =
        previous_.pressure * (1.0 - alpha)
        + input.pressure * alpha;

    output.tiltX =
        previous_.tiltX * (1.0 - alpha)
        + input.tiltX * alpha;

    output.tiltY =
        previous_.tiltY * (1.0 - alpha)
        + input.tiltY * alpha;

    output.rotation =
        previous_.rotation * (1.0 - alpha)
        + input.rotation * alpha;

    output.timestamp =
        input.timestamp;

    previous_ = output;

    return output;
}

void PenFilter::reset()
{
    initialized_ = false;
    previous_ = {};
}

}
include/PenPredictor.hpp
#pragma once

#include "PenTypes.hpp"

namespace surface::pen {

class PenPredictor
{
public:

    explicit PenPredictor(
        double predictionMilliseconds = 8.0);

    PredictedPoint predict(
        const PenPoint& current,
        const PenVelocity& velocity) const;

private:

    double predictionMilliseconds_;
};

}
src/PenPredictor.cpp
#include "PenPredictor.hpp"

#include <algorithm>

namespace surface::pen {

PenPredictor::PenPredictor(
    double predictionMilliseconds)
    : predictionMilliseconds_(
        std::clamp(
            predictionMilliseconds,
            0.0,
            30.0))
{
}

PredictedPoint PenPredictor::predict(
    const PenPoint& current,
    const PenVelocity& velocity) const
{
    const double seconds =
        predictionMilliseconds_
        / 1000.0;

    PredictedPoint result;

    result.x =
        current.x +
        velocity.x * seconds;

    result.y =
        current.y +
        velocity.y * seconds;

    result.pressure =
        current.pressure;

    result.tiltX =
        current.tiltX;

    result.tiltY =
        current.tiltY;

    result.timestamp =
        current.timestamp;

    return result;
}

}
include/PalmRejection.hpp
#pragma once

#include "PenTypes.hpp"

namespace surface::pen {

class PalmRejection
{
public:

    explicit PalmRejection(
        double widthThreshold = 35.0,
        double heightThreshold = 35.0);

    bool isPalm(
        const PalmContact& contact) const;

    bool shouldReject(
        const PalmContact& contact,
        const PenState& pen) const;

private:

    double widthThreshold_;
    double heightThreshold_;
};

}
src/PalmRejection.cpp
#include "PalmRejection.hpp"

namespace surface::pen {

PalmRejection::PalmRejection(
    double widthThreshold,
    double heightThreshold)
    : widthThreshold_(widthThreshold),
      heightThreshold_(heightThreshold)
{
}

bool PalmRejection::isPalm(
    const PalmContact& contact) const
{
    if (!contact.active)
        return false;

    return
        contact.width >= widthThreshold_ &&
        contact.height >= heightThreshold_;
}

bool PalmRejection::shouldReject(
    const PalmContact& contact,
    const PenState& pen) const
{
    /*
        When a pen is actively drawing, large nearby
        contacts can be treated as palm contacts.

        A real production implementation should combine
        contact geometry, device type, pen proximity,
        contact history and Windows digitizer metadata.
    */

    if (!pen.inRange)
        return false;

    if (!contact.active)
        return false;

    return isPalm(contact);
}

}
include/PenEngine.hpp
#pragma once

#include "PenTypes.hpp"
#include "PenFilter.hpp"
#include "PenPredictor.hpp"
#include "PalmRejection.hpp"

#include <functional>
#include <mutex>

namespace surface::pen {

class PenEngine
{
public:

    using OutputCallback =
        std::function<void(
            const PredictedPoint&)>;

    explicit PenEngine(
        PenConfiguration configuration = {});

    void setOutputCallback(
        OutputCallback callback);

    void processPenState(
        PenState state);

    void processPalmContact(
        PalmContact contact);

    void reset();

private:

    PenVelocity calculateVelocity(
        const PenPoint& current);

    PenConfiguration configuration_;

    PenFilter filter_;
    PenPredictor predictor_;
    PalmRejection palmRejection_;

    OutputCallback callback_;

    PenPoint previousPoint_;
    bool previousPointValid_{false};

    PalmContact currentPalm_;

    std::mutex mutex_;
};

}
src/PenEngine.cpp
#include "PenEngine.hpp"

#include <algorithm>
#include <chrono>

namespace surface::pen {

PenEngine::PenEngine(
    PenConfiguration configuration)
    : configuration_(configuration),
      filter_(
          configuration.smoothingFactor),
      predictor_(
          configuration.predictionMilliseconds),
      palmRejection_(
          configuration.palmWidthThreshold,
          configuration.palmHeightThreshold)
{
}

void PenEngine::setOutputCallback(
    OutputCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}

PenVelocity PenEngine::calculateVelocity(
    const PenPoint& current)
{
    PenVelocity velocity{};

    if (!previousPointValid_)
    {
        previousPoint_ = current;
        previousPointValid_ = true;

        return velocity;
    }

    const double seconds =
        std::chrono::duration<double>(
            current.timestamp -
            previousPoint_.timestamp
        ).count();

    if (seconds <= 0.000001)
        return velocity;

    velocity.x =
        (current.x - previousPoint_.x)
        / seconds;

    velocity.y =
        (current.y - previousPoint_.y)
        / seconds;

    /*
        Prevent pathological velocity spikes from
        corrupting prediction after missed samples.
    */

    constexpr double maxVelocity =
        10000.0;

    velocity.x =
        std::clamp(
            velocity.x,
            -maxVelocity,
            maxVelocity
        );

    velocity.y =
        std::clamp(
            velocity.y,
            -maxVelocity,
            maxVelocity
        );

    previousPoint_ = current;

    return velocity;
}

void PenEngine::processPenState(
    PenState state)
{
    OutputCallback callback;

    {
        std::lock_guard lock(mutex_);

        if (!state.inRange)
        {
            previousPointValid_ = false;
            filter_.reset();

            return;
        }

        /*
            Palm rejection is only relevant when a
            physical contact is being interpreted alongside
            active pen interaction.
        */

        if (
            currentPalm_.active &&
            palmRejection_.shouldReject(
                currentPalm_,
                state))
        {
            return;
        }

        PenPoint filtered =
            filter_.filter(
                state.point);

        PenVelocity velocity =
            calculateVelocity(
                filtered);

        PredictedPoint predicted =
            predictor_.predict(
                filtered,
                velocity);

        callback =
            callback_;
        
        if (!state.tipDown)
        {
            predicted.pressure = 0.0;
        }
    }

    if (callback)
    {
        callback(predicted);
    }
}

void PenEngine::processPalmContact(
    PalmContact contact)
{
    std::lock_guard lock(mutex_);

    currentPalm_ =
        contact;
}

void PenEngine::reset()
{
    std::lock_guard lock(mutex_);

    previousPointValid_ = false;

    previousPoint_ = {};

    currentPalm_ = {};

    filter_.reset();
}

}
src/main.cpp
#include "PenEngine.hpp"
#include "WindowsPenInput.hpp"

#include <iostream>

int main()
{
    using namespace surface::pen;

    PenEngine engine;

    engine.setOutputCallback(
        [](const PredictedPoint& point)
        {
            std::cout
                << "Pen: "
                << "X=" << point.x
                << " Y=" << point.y
                << " Pressure=" << point.pressure
                << " TiltX=" << point.tiltX
                << " TiltY=" << point.tiltY
                << '\n';
        }
    );

    WindowsPenInput input;

    input.setCallback(
        [&](const PenState& state)
        {
            engine.processPenState(state);
        }
    );

    std::cout
        << "Surface Pen Engine initialized.\n"
        << "Waiting for Windows pen input...\n";

    /*
        In a real Win32/WinUI host, WindowsPenInput::processPointerMessage()
        would be called from the application's pointer-message handling.

        This executable intentionally does not fabricate a window/input
        subsystem simply to make the demonstration appear functional.
    */

    std::cout
        << "Press ENTER to exit.\n";

    std::cin.get();

    engine.reset();

    return 0;
}
tests/PenTests.cpp
#include "PenEngine.hpp"
#include "PenFilter.hpp"
#include "PenPredictor.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace surface::pen;

int main()
{
    /*
        Filter test.
    */

    PenFilter filter(0.5);

    PenPoint first;

    first.x = 0.0;
    first.y = 0.0;

    filter.filter(first);

    PenPoint second;

    second.x = 10.0;
    second.y = 20.0;

    PenPoint filtered =
        filter.filter(second);

    assert(filtered.x > 0.0);
    assert(filtered.x < 10.0);

    assert(filtered.y > 0.0);
    assert(filtered.y < 20.0);

    /*
        Prediction test.
    */

    PenPredictor predictor(10.0);

    PenPoint point;

    point.x = 100.0;
    point.y = 100.0;

    PenVelocity velocity;

    velocity.x = 1000.0;
    velocity.y = 500.0;

    PredictedPoint prediction =
        predictor.predict(
            point,
            velocity
        );

    assert(prediction.x > point.x);
    assert(prediction.y > point.y);

    /*
        Palm rejection test.
    */

    PalmRejection rejection(
        35.0,
        35.0
    );

    PalmContact palm;

    palm.active = true;
    palm.width = 100.0;
    palm.height = 80.0;

    PenState pen;

    pen.inRange = true;

    assert(
        rejection.shouldReject(
            palm,
            pen
        )
    );

    std::cout
        << "All Surface Pen tests passed.\n";

    return 0;
}
CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfacePenEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfacePenEngine

    src/main.cpp
    src/PenFilter.cpp
    src/PenPredictor.cpp
    src/PalmRejection.cpp
    src/PenEngine.cpp
    src/WindowsPenInput.cpp
)

target_include_directories(
    SurfacePenEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfacePenEngine
        PRIVATE

        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )

endif()

enable_testing()

add_executable(
    PenTests

    tests/PenTests.cpp
    src/PenFilter.cpp
    src/PenPredictor.cpp
    src/PalmRejection.cpp
    src/PenEngine.cpp
)

target_include_directories(
    PenTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME PenTests
    COMMAND PenTests
)










#4 — Surface Touchscreen Engine

This one builds on the Pen Engine but treats multi-touch as its own native C++ subsystem: contact tracking, palm rejection, gesture recognition, smoothing, velocity estimation, and low-latency touch prediction.

Project structure
SurfaceTouchEngine/
├── CMakeLists.txt
├── include/
│   ├── TouchTypes.hpp
│   ├── TouchFilter.hpp
│   ├── TouchTracker.hpp
│   ├── GestureEngine.hpp
│   ├── TouchPalmRejection.hpp
│   ├── TouchEngine.hpp
│   └── WindowsTouchInput.hpp
├── src/
│   ├── TouchFilter.cpp
│   ├── TouchTracker.cpp
│   ├── GestureEngine.cpp
│   ├── TouchPalmRejection.cpp
│   ├── TouchEngine.cpp
│   ├── WindowsTouchInput.cpp
│   └── main.cpp
└── tests/
    └── TouchTests.cpp
include/TouchTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace surface::touch {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

using ContactId = std::uint32_t;

enum class ContactType
{
    Finger,
    Palm,
    Stylus,
    Unknown
};

enum class TouchPhase
{
    Down,
    Move,
    Up,
    Cancel
};

enum class GestureType
{
    None,
    Tap,
    DoubleTap,
    LongPress,
    Swipe,
    Pinch,
    Rotate,
    TwoFingerPan,
    ThreeFingerSwipe
};

struct TouchPoint
{
    ContactId id{0};

    double x{0.0};
    double y{0.0};

    double width{0.0};
    double height{0.0};

    double pressure{0.0};

    ContactType type{
        ContactType::Unknown
    };

    TouchPhase phase{
        TouchPhase::Move
    };

    TimePoint timestamp{
        Clock::now()
    };
};

struct TouchVelocity
{
    double x{0.0};
    double y{0.0};
};

struct TrackedContact
{
    TouchPoint point;

    TouchVelocity velocity;

    double startX{0.0};
    double startY{0.0};

    double distanceTravelled{0.0};

    TimePoint startTime{
        Clock::now()
    };

    bool active{true};
};

struct PredictedTouch
{
    ContactId id{0};

    double x{0.0};
    double y{0.0};

    double pressure{0.0};

    TimePoint timestamp{
        Clock::now()
    };
};

struct Gesture
{
    GestureType type{
        GestureType::None
    };

    ContactId primaryContact{0};

    double deltaX{0.0};
    double deltaY{0.0};

    double scale{1.0};

    double rotationRadians{0.0};

    unsigned contactCount{0};

    bool valid{false};
};

struct TouchConfiguration
{
    double smoothingFactor{0.30};

    double predictionMilliseconds{8.0};

    double palmWidthThreshold{40.0};
    double palmHeightThreshold{40.0};

    double tapMaximumMovement{12.0};

    double tapMaximumDurationMs{250.0};

    double longPressDurationMs{600.0};

    double swipeMinimumDistance{80.0};

    double pinchMinimumScaleChange{0.05};
};

struct TouchFrame
{
    std::vector<TouchPoint> contacts;

    TimePoint timestamp{
        Clock::now()
    };
};

}
include/TouchFilter.hpp
#pragma once

#include "TouchTypes.hpp"

#include <unordered_map>

namespace surface::touch {

class TouchFilter
{
public:

    explicit TouchFilter(
        double smoothingFactor = 0.30);

    TouchPoint filter(
        const TouchPoint& input);

    void reset(
        ContactId id);

    void resetAll();

private:

    struct PreviousPoint
    {
        TouchPoint point;
        bool valid{false};
    };

    double smoothingFactor_;

    std::unordered_map<
        ContactId,
        PreviousPoint
    > previous_;
};

}
src/TouchFilter.cpp
#include "TouchFilter.hpp"

#include <algorithm>

namespace surface::touch {

TouchFilter::TouchFilter(
    double smoothingFactor)
    : smoothingFactor_(
        std::clamp(
            smoothingFactor,
            0.0,
            1.0))
{
}

TouchPoint TouchFilter::filter(
    const TouchPoint& input)
{
    auto& previous =
        previous_[input.id];

    if (!previous.valid)
    {
        previous.point = input;
        previous.valid = true;

        return input;
    }

    const double alpha =
        smoothingFactor_;

    TouchPoint output = input;

    output.x =
        previous.point.x * (1.0 - alpha)
        + input.x * alpha;

    output.y =
        previous.point.y * (1.0 - alpha)
        + input.y * alpha;

    output.width =
        previous.point.width * (1.0 - alpha)
        + input.width * alpha;

    output.height =
        previous.point.height * (1.0 - alpha)
        + input.height * alpha;

    output.pressure =
        previous.point.pressure * (1.0 - alpha)
        + input.pressure * alpha;

    previous.point = output;

    return output;
}

void TouchFilter::reset(
    ContactId id)
{
    previous_.erase(id);
}

void TouchFilter::resetAll()
{
    previous_.clear();
}

}
include/TouchTracker.hpp
#pragma once

#include "TouchTypes.hpp"

#include <unordered_map>
#include <vector>

namespace surface::touch {

class TouchTracker
{
public:

    void update(
        const TouchPoint& point);

    void remove(
        ContactId id);

    void clear();

    bool contains(
        ContactId id) const;

    const TrackedContact* get(
        ContactId id) const;

    std::vector<TrackedContact>
    activeContacts() const;

private:

    std::unordered_map<
        ContactId,
        TrackedContact
    > contacts_;
};

}
src/TouchTracker.cpp
#include "TouchTracker.hpp"

#include <algorithm>
#include <cmath>

namespace surface::touch {

void TouchTracker::update(
    const TouchPoint& point)
{
    auto iterator =
        contacts_.find(point.id);

    if (iterator == contacts_.end())
    {
        TrackedContact contact;

        contact.point = point;

        contact.startX = point.x;
        contact.startY = point.y;

        contact.startTime =
            point.timestamp;

        contact.active =
            point.phase != TouchPhase::Up &&
            point.phase != TouchPhase::Cancel;

        contacts_.emplace(
            point.id,
            contact
        );

        return;
    }

    TrackedContact& contact =
        iterator->second;

    const double oldX =
        contact.point.x;

    const double oldY =
        contact.point.y;

    const double dt =
        std::chrono::duration<double>(
            point.timestamp -
            contact.point.timestamp
        ).count();

    if (dt > 0.000001)
    {
        contact.velocity.x =
            (point.x - oldX) / dt;

        contact.velocity.y =
            (point.y - oldY) / dt;
    }

    const double dx =
        point.x - oldX;

    const double dy =
        point.y - oldY;

    contact.distanceTravelled +=
        std::sqrt(
            dx * dx +
            dy * dy
        );

    contact.point = point;

    contact.active =
        point.phase != TouchPhase::Up &&
        point.phase != TouchPhase::Cancel;
}

void TouchTracker::remove(
    ContactId id)
{
    contacts_.erase(id);
}

void TouchTracker::clear()
{
    contacts_.clear();
}

bool TouchTracker::contains(
    ContactId id) const
{
    return contacts_.contains(id);
}

const TrackedContact*
TouchTracker::get(
    ContactId id) const
{
    const auto iterator =
        contacts_.find(id);

    if (iterator ==
        contacts_.end())
    {
        return nullptr;
    }

    return &iterator->second;
}

std::vector<TrackedContact>
TouchTracker::activeContacts() const
{
    std::vector<TrackedContact> result;

    for (const auto& [id, contact] :
         contacts_)
    {
        if (contact.active)
        {
            result.push_back(contact);
        }
    }

    return result;
}

}
include/TouchPalmRejection.hpp
#pragma once

#include "TouchTypes.hpp"

namespace surface::touch {

class TouchPalmRejection
{
public:

    TouchPalmRejection(
        double widthThreshold,
        double heightThreshold);

    bool isPalm(
        const TouchPoint& point) const;

    bool shouldReject(
        const TouchPoint& point) const;

private:

    double widthThreshold_;
    double heightThreshold_;
};

}
src/TouchPalmRejection.cpp
#include "TouchPalmRejection.hpp"

namespace surface::touch {

TouchPalmRejection::TouchPalmRejection(
    double widthThreshold,
    double heightThreshold)
    : widthThreshold_(widthThreshold),
      heightThreshold_(heightThreshold)
{
}

bool TouchPalmRejection::isPalm(
    const TouchPoint& point) const
{
    if (point.type == ContactType::Palm)
    {
        return true;
    }

    return
        point.width >= widthThreshold_ &&
        point.height >= heightThreshold_;
}

bool TouchPalmRejection::shouldReject(
    const TouchPoint& point) const
{
    return isPalm(point);
}

}
include/GestureEngine.hpp
#pragma once

#include "TouchTypes.hpp"
#include "TouchTracker.hpp"

#include <vector>

namespace surface::touch {

class GestureEngine
{
public:

    explicit GestureEngine(
        TouchConfiguration configuration = {});

    Gesture process(
        const TouchTracker& tracker);

private:

    Gesture detectSingleContact(
        const TrackedContact& contact) const;

    Gesture detectTwoFingerGesture(
        const std::vector<TrackedContact>& contacts) const;

    Gesture detectThreeFingerGesture(
        const std::vector<TrackedContact>& contacts) const;

    TouchConfiguration configuration_;
};

}
src/GestureEngine.cpp
#include "GestureEngine.hpp"

#include <algorithm>
#include <cmath>

namespace surface::touch {

GestureEngine::GestureEngine(
    TouchConfiguration configuration)
    : configuration_(configuration)
{
}

Gesture GestureEngine::process(
    const TouchTracker& tracker)
{
    const auto contacts =
        tracker.activeContacts();

    if (contacts.empty())
    {
        return {};
    }

    if (contacts.size() == 1)
    {
        return detectSingleContact(
            contacts.front()
        );
    }

    if (contacts.size() == 2)
    {
        return detectTwoFingerGesture(
            contacts
        );
    }

    if (contacts.size() == 3)
    {
        return detectThreeFingerGesture(
            contacts
        );
    }

    return {};
}

Gesture GestureEngine::detectSingleContact(
    const TrackedContact& contact) const
{
    Gesture gesture;

    gesture.primaryContact =
        contact.point.id;

    gesture.contactCount = 1;

    const double dx =
        contact.point.x -
        contact.startX;

    const double dy =
        contact.point.y -
        contact.startY;

    const double distance =
        std::sqrt(
            dx * dx +
            dy * dy
        );

    const double durationMs =
        std::chrono::duration<double, std::milli>(
            contact.point.timestamp -
            contact.startTime
        ).count();

    if (
        contact.point.phase == TouchPhase::Up &&
        distance <=
            configuration_.tapMaximumMovement &&
        durationMs <=
            configuration_.tapMaximumDurationMs)
    {
        gesture.type =
            GestureType::Tap;

        gesture.valid = true;

        return gesture;
    }

    if (
        contact.point.phase == TouchPhase::Move &&
        durationMs >=
            configuration_.longPressDurationMs &&
        distance <=
            configuration_.tapMaximumMovement)
    {
        gesture.type =
            GestureType::LongPress;

        gesture.valid = true;

        return gesture;
    }

    if (
        contact.point.phase == TouchPhase::Move &&
        distance >=
            configuration_.swipeMinimumDistance)
    {
        gesture.type =
            GestureType::Swipe;

        gesture.deltaX = dx;
        gesture.deltaY = dy;

        gesture.valid = true;

        return gesture;
    }

    return gesture;
}

Gesture GestureEngine::detectTwoFingerGesture(
    const std::vector<TrackedContact>& contacts) const
{
    Gesture gesture;

    if (contacts.size() != 2)
        return gesture;

    const auto& a =
        contacts[0];

    const auto& b =
        contacts[1];

    const double currentDx =
        b.point.x -
        a.point.x;

    const double currentDy =
        b.point.y -
        a.point.y;

    const double startDx =
        (b.startX - a.startX);

    const double startDy =
        (b.startY - a.startY);

    const double currentDistance =
        std::sqrt(
            currentDx * currentDx +
            currentDy * currentDy
        );

    const double startDistance =
        std::sqrt(
            startDx * startDx +
            startDy * startDy
        );

    if (startDistance <= 0.001)
        return gesture;

    const double scale =
        currentDistance /
        startDistance;

    if (
        std::abs(
            scale - 1.0
        ) >=
        configuration_.pinchMinimumScaleChange)
    {
        gesture.type =
            GestureType::Pinch;

        gesture.scale = scale;

        gesture.contactCount = 2;

        gesture.valid = true;

        return gesture;
    }

    const double centerStartX =
        (a.startX + b.startX) / 2.0;

    const double centerStartY =
        (a.startY + b.startY) / 2.0;

    const double centerCurrentX =
        (a.point.x + b.point.x) / 2.0;

    const double centerCurrentY =
        (a.point.y + b.point.y) / 2.0;

    gesture.type =
        GestureType::TwoFingerPan;

    gesture.deltaX =
        centerCurrentX -
        centerStartX;

    gesture.deltaY =
        centerCurrentY -
        centerStartY;

    gesture.contactCount = 2;

    gesture.valid = true;

    return gesture;
}

Gesture GestureEngine::detectThreeFingerGesture(
    const std::vector<TrackedContact>& contacts) const
{
    Gesture gesture;

    if (contacts.size() != 3)
        return gesture;

    double averageDx = 0.0;
    double averageDy = 0.0;

    for (const auto& contact :
         contacts)
    {
        averageDx +=
            contact.point.x -
            contact.startX;

        averageDy +=
            contact.point.y -
            contact.startY;
    }

    averageDx /= 3.0;
    averageDy /= 3.0;

    if (
        std::abs(averageDx) >=
        configuration_.swipeMinimumDistance ||
        std::abs(averageDy) >=
        configuration_.swipeMinimumDistance)
    {
        gesture.type =
            GestureType::ThreeFingerSwipe;

        gesture.deltaX =
            averageDx;

        gesture.deltaY =
            averageDy;

        gesture.contactCount = 3;

        gesture.valid = true;
    }

    return gesture;
}

}
include/TouchEngine.hpp
#pragma once

#include "TouchTypes.hpp"
#include "TouchFilter.hpp"
#include "TouchTracker.hpp"
#include "GestureEngine.hpp"
#include "TouchPalmRejection.hpp"

#include <functional>
#include <mutex>

namespace surface::touch {

class TouchEngine
{
public:

    using TouchCallback =
        std::function<void(
            const PredictedTouch&)>;

    using GestureCallback =
        std::function<void(
            const Gesture&)>;

    explicit TouchEngine(
        TouchConfiguration configuration = {});

    void setTouchCallback(
        TouchCallback callback);

    void setGestureCallback(
        GestureCallback callback);

    void processTouch(
        TouchPoint point);

    void reset();

private:

    PredictedTouch predict(
        const TrackedContact& contact) const;

    TouchConfiguration configuration_;

    TouchFilter filter_;
    TouchTracker tracker_;
    GestureEngine gestureEngine_;
    TouchPalmRejection palmRejection_;

    TouchCallback touchCallback_;
    GestureCallback gestureCallback_;

    mutable std::mutex mutex_;
};

}
src/TouchEngine.cpp
#include "TouchEngine.hpp"

#include <algorithm>

namespace surface::touch {

TouchEngine::TouchEngine(
    TouchConfiguration configuration)
    : configuration_(configuration),
      filter_(
          configuration.smoothingFactor),
      gestureEngine_(
          configuration),
      palmRejection_(
          configuration.palmWidthThreshold,
          configuration.palmHeightThreshold)
{
}

void TouchEngine::setTouchCallback(
    TouchCallback callback)
{
    std::lock_guard lock(mutex_);

    touchCallback_ =
        std::move(callback);
}

void TouchEngine::setGestureCallback(
    GestureCallback callback)
{
    std::lock_guard lock(mutex_);

    gestureCallback_ =
        std::move(callback);
}

void TouchEngine::processTouch(
    TouchPoint point)
{
    TouchCallback touchCallback;
    GestureCallback gestureCallback;

    PredictedTouch predicted;
    Gesture gesture;

    bool emitTouch = false;
    bool emitGesture = false;

    {
        std::lock_guard lock(mutex_);

        /*
            Palm contacts are rejected before entering
            the normal gesture/contact pipeline.
        */

        if (palmRejection_.shouldReject(point))
        {
            return;
        }

        TouchPoint filtered =
            filter_.filter(point);

        tracker_.update(filtered);

        const TrackedContact* contact =
            tracker_.get(
                filtered.id
            );

        if (contact)
        {
            predicted =
                predict(*contact);

            emitTouch = true;
        }

        gesture =
            gestureEngine_.process(
                tracker_
            );

        emitGesture =
            gesture.valid;

        touchCallback =
            touchCallback_;

        gestureCallback =
            gestureCallback_;

        if (
            filtered.phase == TouchPhase::Up ||
            filtered.phase == TouchPhase::Cancel)
        {
            tracker_.remove(
                filtered.id
            );

            filter_.reset(
                filtered.id
            );
        }
    }

    if (
        emitTouch &&
        touchCallback)
    {
        touchCallback(
            predicted
        );
    }

    if (
        emitGesture &&
        gestureCallback)
    {
        gestureCallback(
            gesture
        );
    }
}

PredictedTouch TouchEngine::predict(
    const TrackedContact& contact) const
{
    PredictedTouch result;

    result.id =
        contact.point.id;

    const double seconds =
        configuration_.predictionMilliseconds
        / 1000.0;

    result.x =
        contact.point.x +
        contact.velocity.x *
        seconds;

    result.y =
        contact.point.y +
        contact.velocity.y *
        seconds;

    result.pressure =
        contact.point.pressure;

    result.timestamp =
        contact.point.timestamp;

    return result;
}

void TouchEngine::reset()
{
    std::lock_guard lock(mutex_);

    tracker_.clear();
    filter_.resetAll();
}

}
include/WindowsTouchInput.hpp
#pragma once

#include "TouchTypes.hpp"

#include <windows.h>
#include <functional>

namespace surface::touch {

class WindowsTouchInput
{
public:

    using TouchCallback =
        std::function<void(
            const TouchPoint&)>;

    void setCallback(
        TouchCallback callback);

    void processPointerMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

private:

    TouchCallback callback_;

    TouchPoint decodePointer(
        UINT message,
        WPARAM wParam,
        LPARAM lParam) const;
};

}
src/WindowsTouchInput.cpp
#include "WindowsTouchInput.hpp"

#include <windowsx.h>

namespace surface::touch {

void WindowsTouchInput::setCallback(
    TouchCallback callback)
{
    callback_ =
        std::move(callback);
}

void WindowsTouchInput::processPointerMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (!callback_)
        return;

    TouchPoint point =
        decodePointer(
            message,
            wParam,
            lParam
        );

    callback_(point);
}

TouchPoint
WindowsTouchInput::decodePointer(
    UINT message,
    WPARAM wParam,
    LPARAM lParam) const
{
    TouchPoint point;

    const UINT32 pointerId =
        GET_POINTERID_WPARAM(wParam);

    POINTER_INFO info{};

    if (!GetPointerInfo(
            pointerId,
            &info))
    {
        return point;
    }

    point.id =
        pointerId;

    point.x =
        static_cast<double>(
            info.ptPixelLocation.x
        );

    point.y =
        static_cast<double>(
            info.ptPixelLocation.y
        );

    point.timestamp =
        Clock::now();

    switch (message)
    {
        case WM_POINTERDOWN:
            point.phase =
                TouchPhase::Down;
            break;

        case WM_POINTERUPDATE:
            point.phase =
                TouchPhase::Move;
            break;

        case WM_POINTERUP:
            point.phase =
                TouchPhase::Up;
            break;

        default:
            point.phase =
                TouchPhase::Move;
            break;
    }

    if (info.pointerType == PT_TOUCH)
    {
        point.type =
            ContactType::Finger;

        POINTER_TOUCH_INFO touchInfo{};

        if (GetPointerTouchInfo(
                pointerId,
                &touchInfo))
        {
            const RECT& rect =
                touchInfo.rcContact;

            point.width =
                static_cast<double>(
                    rect.right -
                    rect.left
                );

            point.height =
                static_cast<double>(
                    rect.bottom -
                    rect.top
                );

            point.pressure =
                static_cast<double>(
                    touchInfo.pressure
                ) / 1024.0;
        }
    }
    else if (
        info.pointerType == PT_PEN)
    {
        point.type =
            ContactType::Stylus;
    }
    else
    {
        point.type =
            ContactType::Unknown;
    }

    return point;
}

}
src/main.cpp
#include "TouchEngine.hpp"
#include "WindowsTouchInput.hpp"

#include <iostream>

int main()
{
    using namespace surface::touch;

    TouchEngine engine;

    engine.setTouchCallback(
        [](const PredictedTouch& touch)
        {
            std::cout
                << "[TOUCH] "
                << "ID=" << touch.id
                << " X=" << touch.x
                << " Y=" << touch.y
                << " Pressure=" << touch.pressure
                << '\n';
        }
    );

    engine.setGestureCallback(
        [](const Gesture& gesture)
        {
            std::cout
                << "[GESTURE] "
                << "contacts="
                << gesture.contactCount
                << " scale="
                << gesture.scale
                << " dx="
                << gesture.deltaX
                << " dy="
                << gesture.deltaY
                << '\n';
        }
    );

    WindowsTouchInput windowsInput;

    windowsInput.setCallback(
        [&](const TouchPoint& point)
        {
            engine.processTouch(point);
        }
    );

    std::cout
        << "====================================\n"
        << " Surface Touch Engine\n"
        << " C++20 / Windows\n"
        << "====================================\n";

    std::cout
        << "Touch engine initialized.\n";

    /*
        A Win32/WinUI application's window procedure would forward
        WM_POINTERDOWN, WM_POINTERUPDATE and WM_POINTERUP messages
        to:

            windowsInput.processPointerMessage(...)

        This keeps the touch engine independent of the UI framework.
    */

    std::cout
        << "Press ENTER to exit.\n";

    std::cin.get();

    engine.reset();

    return 0;
}
tests/TouchTests.cpp
#include "TouchFilter.hpp"
#include "TouchTracker.hpp"
#include "GestureEngine.hpp"
#include "TouchPalmRejection.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace surface::touch;

int main()
{
    const auto now =
        Clock::now();

    /*
        Filter.
    */

    TouchFilter filter(0.5);

    TouchPoint a;

    a.id = 1;
    a.x = 0.0;
    a.y = 0.0;
    a.timestamp = now;

    filter.filter(a);

    TouchPoint b = a;

    b.x = 100.0;
    b.y = 50.0;

    b.timestamp =
        now +
        std::chrono::milliseconds(10);

    TouchPoint filtered =
        filter.filter(b);

    assert(filtered.x > 0.0);
    assert(filtered.x < 100.0);

    /*
        Tracker.
    */

    TouchTracker tracker;

    TouchPoint down;

    down.id = 10;
    down.x = 100.0;
    down.y = 100.0;

    down.phase =
        TouchPhase::Down;

    down.timestamp = now;

    tracker.update(down);

    assert(
        tracker.contains(10)
    );

    TouchPoint move = down;

    move.x = 200.0;
    move.y = 100.0;

    move.phase =
        TouchPhase::Move;

    move.timestamp =
        now +
        std::chrono::milliseconds(20);

    tracker.update(move);

    const TrackedContact* contact =
        tracker.get(10);

    assert(contact != nullptr);

    assert(
        contact->distanceTravelled > 0.0
    );

    /*
        Palm rejection.
    */

    TouchPalmRejection palm(
        40.0,
        40.0
    );

    TouchPoint palmPoint;

    palmPoint.width = 100.0;
    palmPoint.height = 80.0;

    assert(
        palm.shouldReject(
            palmPoint
        )
    );

    /*
        Gesture engine.
    */

    TouchConfiguration configuration;

    configuration.swipeMinimumDistance =
        50.0;

    GestureEngine gestures(configuration);

    TouchTracker gestureTracker;

    TouchPoint gestureStart;

    gestureStart.id = 20;

    gestureStart.x = 100.0;
    gestureStart.y = 100.0;

    gestureStart.phase =
        TouchPhase::Down;

    gestureStart.timestamp =
        now;

    gestureTracker.update(
        gestureStart
    );

    TouchPoint gestureMove =
        gestureStart;

    gestureMove.x = 200.0;

    gestureMove.phase =
        TouchPhase::Move;

    gestureMove.timestamp =
        now +
        std::chrono::milliseconds(20);

    gestureTracker.update(
        gestureMove
    );

    Gesture gesture =
        gestures.process(
            gestureTracker
        );

    assert(
        gesture.type ==
        GestureType::Swipe
    );

    std::cout
        << "All Surface Touch tests passed.\n";

    return 0;
}
CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceTouchEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceTouchEngine

    src/main.cpp
    src/TouchFilter.cpp
    src/TouchTracker.cpp
    src/GestureEngine.cpp
    src/TouchPalmRejection.cpp
    src/TouchEngine.cpp
    src/WindowsTouchInput.cpp
)

target_include_directories(
    SurfaceTouchEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceTouchEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    TouchTests

    tests/TouchTests.cpp
    src/TouchFilter.cpp
    src/TouchTracker.cpp
    src/GestureEngine.cpp
    src/TouchPalmRejection.cpp
)

target_include_directories(
    TouchTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME TouchTests
    COMMAND TouchTests
)











Project
SurfaceComputeScheduler/
├── CMakeLists.txt
├── include/
│   ├── ComputeTypes.hpp
│   ├── HardwareTelemetry.hpp
│   ├── WorkloadClassifier.hpp
│   ├── ComputeQueue.hpp
│   ├── SchedulerPolicy.hpp
│   ├── ComputeScheduler.hpp
│   └── WindowsTelemetry.hpp
├── src/
│   ├── HardwareTelemetry.cpp
│   ├── WorkloadClassifier.cpp
│   ├── ComputeQueue.cpp
│   ├── SchedulerPolicy.cpp
│   ├── ComputeScheduler.cpp
│   ├── WindowsTelemetry.cpp
│   └── main.cpp
└── tests/
    └── SchedulerTests.cpp
1. include/ComputeTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace surface::compute {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class ComputeUnit
{
    CPU,
    GPU,
    NPU
};

enum class WorkloadType
{
    General,
    Interactive,
    Graphics,
    MachineLearning,
    Media,
    Background,
    LatencyCritical
};

enum class Priority
{
    Background = 0,
    Low = 1,
    Normal = 2,
    High = 3,
    Critical = 4
};

enum class SchedulerAction
{
    Keep,
    Migrate,
    Throttle,
    Boost,
    Defer
};

struct HardwareState
{
    double cpuUtilization{0.0};
    double gpuUtilization{0.0};
    double npuUtilization{0.0};

    double cpuTemperature{0.0};
    double gpuTemperature{0.0};
    double npuTemperature{0.0};

    double batteryLevel{100.0};

    bool onACPower{true};

    bool cpuAvailable{true};
    bool gpuAvailable{true};
    bool npuAvailable{false};
};

struct Workload
{
    std::uint64_t id{0};

    std::string name;

    WorkloadType type{
        WorkloadType::General
    };

    Priority priority{
        Priority::Normal
    };

    double cpuDemand{0.0};
    double gpuDemand{0.0};
    double npuDemand{0.0};

    double memoryDemandMB{0.0};

    double latencyBudgetMs{16.0};

    bool interactive{false};
    bool preemptible{true};

    TimePoint submittedAt{
        Clock::now()
    };
};

struct SchedulingDecision
{
    std::uint64_t workloadId{0};

    ComputeUnit target{
        ComputeUnit::CPU
    };

    SchedulerAction action{
        SchedulerAction::Keep
    };

    Priority effectivePriority{
        Priority::Normal
    };

    double performanceFactor{1.0};

    std::string reason;
};

struct SchedulerConfiguration
{
    double cpuHighLoad{85.0};
    double gpuHighLoad{90.0};
    double npuHighLoad{90.0};

    double thermalLimit{90.0};

    double batteryBoostThreshold{80.0};

    double interactiveLatencyTargetMs{16.0};

    unsigned maximumQueueSize{256};

    bool allowNPUOffload{true};
    bool allowGPUOffload{true};
};

}
2. include/HardwareTelemetry.hpp
#pragma once

#include "ComputeTypes.hpp"

namespace surface::compute {

class IHardwareTelemetry
{
public:

    virtual ~IHardwareTelemetry() = default;

    virtual HardwareState read() = 0;
};

}
3. include/WindowsTelemetry.hpp
#pragma once

#include "HardwareTelemetry.hpp"

namespace surface::compute {

class WindowsHardwareTelemetry final
    : public IHardwareTelemetry
{
public:

    HardwareState read() override;
};

}
4. src/WindowsTelemetry.cpp
#include "WindowsTelemetry.hpp"

#ifdef _WIN32

#include <windows.h>

#endif

namespace surface::compute {

HardwareState
WindowsHardwareTelemetry::read()
{
    HardwareState state;

#ifdef _WIN32

    SYSTEM_POWER_STATUS powerStatus{};

    if (GetSystemPowerStatus(&powerStatus))
    {
        state.batteryLevel =
            static_cast<double>(
                powerStatus.BatteryLifePercent
            );

        state.onACPower =
            powerStatus.ACLineStatus == 1;
    }

#endif

    /*
        CPU/GPU/NPU utilisation is intentionally not
        fabricated here.

        A production Surface implementation should
        obtain telemetry through supported Windows
        performance-counter, driver, ETW, DXGI,
        vendor, or platform-specific interfaces.

        These defaults allow the scheduling engine
        to run safely without pretending to have
        hardware telemetry that it does not actually
        possess.
    */

    state.cpuUtilization = 0.0;
    state.gpuUtilization = 0.0;
    state.npuUtilization = 0.0;

    state.cpuAvailable = true;
    state.gpuAvailable = true;

    /*
        Detecting NPU availability should be connected
        to the actual Windows ML / DirectML / driver
        capability layer in a deployment.
    */

    state.npuAvailable = false;

    return state;
}

}
5. include/WorkloadClassifier.hpp
#pragma once

#include "ComputeTypes.hpp"

namespace surface::compute {

class WorkloadClassifier
{
public:

    WorkloadType classify(
        const Workload& workload) const;

    ComputeUnit preferredUnit(
        const Workload& workload,
        const HardwareState& hardware) const;
};

}
6. src/WorkloadClassifier.cpp
#include "WorkloadClassifier.hpp"

#include <algorithm>

namespace surface::compute {

WorkloadType
WorkloadClassifier::classify(
    const Workload& workload) const
{
    if (workload.interactive)
    {
        return WorkloadType::Interactive;
    }

    if (workload.npuDemand >
        workload.cpuDemand &&
        workload.npuDemand >
        workload.gpuDemand)
    {
        return WorkloadType::MachineLearning;
    }

    if (workload.gpuDemand >
        workload.cpuDemand * 1.5)
    {
        return WorkloadType::Graphics;
    }

    if (workload.cpuDemand > 75.0)
    {
        return WorkloadType::General;
    }

    return workload.type;
}

ComputeUnit
WorkloadClassifier::preferredUnit(
    const Workload& workload,
    const HardwareState& hardware) const
{
    const auto type =
        classify(workload);

    switch (type)
    {
        case WorkloadType::Graphics:

            if (hardware.gpuAvailable)
            {
                return ComputeUnit::GPU;
            }

            return ComputeUnit::CPU;

        case WorkloadType::MachineLearning:

            if (hardware.npuAvailable &&
                workload.npuDemand > 0.0)
            {
                return ComputeUnit::NPU;
            }

            if (hardware.gpuAvailable &&
                workload.gpuDemand > 0.0)
            {
                return ComputeUnit::GPU;
            }

            return ComputeUnit::CPU;

        case WorkloadType::Interactive:

        case WorkloadType::LatencyCritical:

            return ComputeUnit::CPU;

        case WorkloadType::Media:

            if (hardware.gpuAvailable)
            {
                return ComputeUnit::GPU;
            }

            return ComputeUnit::CPU;

        case WorkloadType::Background:

        case WorkloadType::General:

        default:

            return ComputeUnit::CPU;
    }
}

}
7. include/ComputeQueue.hpp
#pragma once

#include "ComputeTypes.hpp"

#include <functional>
#include <mutex>
#include <queue>
#include <vector>

namespace surface::compute {

class ComputeQueue
{
public:

    explicit ComputeQueue(
        unsigned maximumSize = 256);

    bool push(
        Workload workload);

    bool pop(
        Workload& workload);

    bool empty() const;

    std::size_t size() const;

    void clear();

    std::vector<Workload>
    snapshot() const;

private:

    struct WorkloadComparator
    {
        bool operator()(
            const Workload& a,
            const Workload& b) const;
    };

    unsigned maximumSize_;

    mutable std::mutex mutex_;

    std::priority_queue<
        Workload,
        std::vector<Workload>,
        WorkloadComparator
    > queue_;
};

}
8. src/ComputeQueue.cpp
#include "ComputeQueue.hpp"

namespace surface::compute {

ComputeQueue::ComputeQueue(
    unsigned maximumSize)
    : maximumSize_(maximumSize)
{
}

bool ComputeQueue::push(
    Workload workload)
{
    std::lock_guard lock(mutex_);

    if (queue_.size() >= maximumSize_)
    {
        return false;
    }

    queue_.push(
        std::move(workload)
    );

    return true;
}

bool ComputeQueue::pop(
    Workload& workload)
{
    std::lock_guard lock(mutex_);

    if (queue_.empty())
    {
        return false;
    }

    workload =
        queue_.top();

    queue_.pop();

    return true;
}

bool ComputeQueue::empty() const
{
    std::lock_guard lock(mutex_);

    return queue_.empty();
}

std::size_t ComputeQueue::size() const
{
    std::lock_guard lock(mutex_);

    return queue_.size();
}

void ComputeQueue::clear()
{
    std::lock_guard lock(mutex_);

    queue_ = {};
}

std::vector<Workload>
ComputeQueue::snapshot() const
{
    std::lock_guard lock(mutex_);

    auto copy = queue_;

    std::vector<Workload> result;

    while (!copy.empty())
    {
        result.push_back(
            copy.top()
        );

        copy.pop();
    }

    return result;
}

bool ComputeQueue::WorkloadComparator::operator()(
    const Workload& a,
    const Workload& b) const
{
    if (a.priority != b.priority)
    {
        return
            static_cast<int>(a.priority) <
            static_cast<int>(b.priority);
    }

    return a.submittedAt >
           b.submittedAt;
}

}
9. include/SchedulerPolicy.hpp
#pragma once

#include "ComputeTypes.hpp"
#include "WorkloadClassifier.hpp"

namespace surface::compute {

class SchedulerPolicy
{
public:

    explicit SchedulerPolicy(
        SchedulerConfiguration configuration = {});

    SchedulingDecision decide(
        const Workload& workload,
        const HardwareState& hardware) const;

private:

    double thermalFactor(
        double temperature) const;

    double utilizationFactor(
        double utilization) const;

    Priority adjustPriority(
        const Workload& workload,
        const HardwareState& hardware) const;

    SchedulerConfiguration configuration_;

    WorkloadClassifier classifier_;
};

}
10. src/SchedulerPolicy.cpp
#include "SchedulerPolicy.hpp"

#include <algorithm>
#include <sstream>

namespace surface::compute {

SchedulerPolicy::SchedulerPolicy(
    SchedulerConfiguration configuration)
    : configuration_(configuration)
{
}

double SchedulerPolicy::thermalFactor(
    double temperature) const
{
    if (temperature >=
        configuration_.thermalLimit)
    {
        return 0.50;
    }

    if (temperature >=
        configuration_.thermalLimit - 10.0)
    {
        return 0.75;
    }

    return 1.0;
}

double SchedulerPolicy::utilizationFactor(
    double utilization) const
{
    if (utilization >= 95.0)
    {
        return 0.60;
    }

    if (utilization >= 85.0)
    {
        return 0.80;
    }

    return 1.0;
}

Priority SchedulerPolicy::adjustPriority(
    const Workload& workload,
    const HardwareState& hardware) const
{
    Priority result =
        workload.priority;

    if (workload.interactive)
    {
        result =
            Priority::High;
    }

    if (
        workload.latencyBudgetMs <=
        configuration_.interactiveLatencyTargetMs)
    {
        if (
            static_cast<int>(result) <
            static_cast<int>(Priority::High))
        {
            result =
                Priority::High;
        }
    }

    if (
        !hardware.onACPower &&
        workload.type ==
            WorkloadType::Background)
    {
        result =
            Priority::Low;
    }

    return result;
}

SchedulingDecision
SchedulerPolicy::decide(
    const Workload& workload,
    const HardwareState& hardware) const
{
    SchedulingDecision decision;

    decision.workloadId =
        workload.id;

    decision.effectivePriority =
        adjustPriority(
            workload,
            hardware
        );

    ComputeUnit target =
        classifier_.preferredUnit(
            workload,
            hardware
        );

    decision.target = target;

    double utilization = 0.0;
    double temperature = 0.0;

    switch (target)
    {
        case ComputeUnit::CPU:

            utilization =
                hardware.cpuUtilization;

            temperature =
                hardware.cpuTemperature;

            break;

        case ComputeUnit::GPU:

            utilization =
                hardware.gpuUtilization;

            temperature =
                hardware.gpuTemperature;

            break;

        case ComputeUnit::NPU:

            utilization =
                hardware.npuUtilization;

            temperature =
                hardware.npuTemperature;

            break;
    }

    const double loadFactor =
        utilizationFactor(
            utilization
        );

    const double tempFactor =
        thermalFactor(
            temperature
        );

    decision.performanceFactor =
        std::min(
            loadFactor,
            tempFactor
        );

    if (
        temperature >=
        configuration_.thermalLimit)
    {
        decision.action =
            SchedulerAction::Throttle;

        decision.reason =
            "Thermal protection";
    }
    else if (
        utilization >=
        configuration_.cpuHighLoad &&
        target == ComputeUnit::CPU &&
        configuration_.allowGPUOffload &&
        hardware.gpuAvailable &&
        workload.gpuDemand > 0.0)
    {
        decision.action =
            SchedulerAction::Migrate;

        decision.target =
            ComputeUnit::GPU;

        decision.reason =
            "CPU saturation with GPU-capable workload";
    }
    else if (
        target == ComputeUnit::NPU &&
        !hardware.npuAvailable)
    {
        decision.action =
            SchedulerAction::Migrate;

        decision.target =
            hardware.gpuAvailable
                ? ComputeUnit::GPU
                : ComputeUnit::CPU;

        decision.reason =
            "NPU unavailable";
    }
    else if (
        workload.interactive &&
        decision.performanceFactor >= 0.75)
    {
        decision.action =
            SchedulerAction::Boost;

        decision.reason =
            "Interactive latency priority";
    }
    else if (
        !hardware.onACPower &&
        workload.type ==
            WorkloadType::Background)
    {
        decision.action =
            SchedulerAction::Defer;

        decision.performanceFactor =
            std::min(
                decision.performanceFactor,
                0.70
            );

        decision.reason =
            "Battery conservation";
    }
    else
    {
        decision.action =
            SchedulerAction::Keep;

        decision.reason =
            "Normal scheduling";
    }

    return decision;
}

}
11. include/ComputeScheduler.hpp
#pragma once

#include "ComputeQueue.hpp"
#include "HardwareTelemetry.hpp"
#include "SchedulerPolicy.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace surface::compute {

class ComputeScheduler
{
public:

    using DecisionCallback =
        std::function<void(
            const SchedulingDecision&)>;

    explicit ComputeScheduler(
        std::unique_ptr<IHardwareTelemetry>
            telemetry,
        SchedulerConfiguration configuration = {});

    ~ComputeScheduler();

    bool submit(
        Workload workload);

    void start();

    void stop();

    HardwareState hardwareState() const;

    std::vector<Workload>
    pendingWorkloads() const;

    void setDecisionCallback(
        DecisionCallback callback);

private:

    void schedulerLoop();

    void schedulePendingWork();

    std::unique_ptr<
        IHardwareTelemetry
    > telemetry_;

    ComputeQueue queue_;

    SchedulerPolicy policy_;

    std::atomic<bool> running_{false};

    std::thread worker_;

    mutable std::mutex stateMutex_;

    HardwareState latestHardwareState_;

    DecisionCallback callback_;
};

}
12. src/ComputeScheduler.cpp
#include "ComputeScheduler.hpp"

#include <chrono>
#include <iostream>

namespace surface::compute {

ComputeScheduler::ComputeScheduler(
    std::unique_ptr<IHardwareTelemetry>
        telemetry,
    SchedulerConfiguration configuration)
    : telemetry_(std::move(telemetry)),
      queue_(configuration.maximumQueueSize),
      policy_(configuration)
{
}

ComputeScheduler::~ComputeScheduler()
{
    stop();
}

bool ComputeScheduler::submit(
    Workload workload)
{
    return queue_.push(
        std::move(workload)
    );
}

void ComputeScheduler::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    worker_ =
        std::thread(
            &ComputeScheduler::schedulerLoop,
            this
        );
}

void ComputeScheduler::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }
}

HardwareState
ComputeScheduler::hardwareState() const
{
    std::lock_guard lock(stateMutex_);

    return latestHardwareState_;
}

std::vector<Workload>
ComputeScheduler::pendingWorkloads() const
{
    return queue_.snapshot();
}

void ComputeScheduler::setDecisionCallback(
    DecisionCallback callback)
{
    std::lock_guard lock(stateMutex_);

    callback_ =
        std::move(callback);
}

void ComputeScheduler::schedulerLoop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        if (telemetry_)
        {
            HardwareState state =
                telemetry_->read();

            {
                std::lock_guard lock(
                    stateMutex_
                );

                latestHardwareState_ =
                    state;
            }
        }

        schedulePendingWork();

        std::this_thread::sleep_for(
            20ms
        );
    }
}

void ComputeScheduler::schedulePendingWork()
{
    Workload workload;

    while (queue_.pop(workload))
    {
        HardwareState state;

        DecisionCallback callback;

        {
            std::lock_guard lock(
                stateMutex_
            );

            state =
                latestHardwareState_;

            callback =
                callback_;
        }

        SchedulingDecision decision =
            policy_.decide(
                workload,
                state
            );

        if (callback)
        {
            callback(decision);
        }
        else
        {
            std::cout
                << "[SCHEDULER] workload="
                << workload.id
                << " target=";

            switch (decision.target)
            {
                case ComputeUnit::CPU:
                    std::cout << "CPU";
                    break;

                case ComputeUnit::GPU:
                    std::cout << "GPU";
                    break;

                case ComputeUnit::NPU:
                    std::cout << "NPU";
                    break;
            }

            std::cout
                << " action=";

            switch (decision.action)
            {
                case SchedulerAction::Keep:
                    std::cout << "KEEP";
                    break;

                case SchedulerAction::Migrate:
                    std::cout << "MIGRATE";
                    break;

                case SchedulerAction::Throttle:
                    std::cout << "THROTTLE";
                    break;

                case SchedulerAction::Boost:
                    std::cout << "BOOST";
                    break;

                case SchedulerAction::Defer:
                    std::cout << "DEFER";
                    break;
            }

            std::cout
                << " factor="
                << decision.performanceFactor
                << " reason="
                << decision.reason
                << '\n';
        }
    }
}

}
13. src/main.cpp
#include "ComputeScheduler.hpp"
#include "WindowsTelemetry.hpp"

#include <iostream>
#include <memory>

using namespace surface::compute;

int main()
{
    auto telemetry =
        std::make_unique<
            WindowsHardwareTelemetry
        >();

    SchedulerConfiguration configuration;

    configuration.allowNPUOffload = true;
    configuration.allowGPUOffload = true;

    ComputeScheduler scheduler(
        std::move(telemetry),
        configuration
    );

    scheduler.setDecisionCallback(
        [](const SchedulingDecision& decision)
        {
            std::cout
                << "\n[DECISION]\n"
                << "Workload: "
                << decision.workloadId
                << "\nTarget: ";

            switch (decision.target)
            {
                case ComputeUnit::CPU:
                    std::cout << "CPU";
                    break;

                case ComputeUnit::GPU:
                    std::cout << "GPU";
                    break;

                case ComputeUnit::NPU:
                    std::cout << "NPU";
                    break;
            }

            std::cout
                << "\nAction: ";

            switch (decision.action)
            {
                case SchedulerAction::Keep:
                    std::cout << "KEEP";
                    break;

                case SchedulerAction::Migrate:
                    std::cout << "MIGRATE";
                    break;

                case SchedulerAction::Throttle:
                    std::cout << "THROTTLE";
                    break;

                case SchedulerAction::Boost:
                    std::cout << "BOOST";
                    break;

                case SchedulerAction::Defer:
                    std::cout << "DEFER";
                    break;
            }

            std::cout
                << "\nPerformance factor: "
                << decision.performanceFactor
                << "\nReason: "
                << decision.reason
                << "\n";
        }
    );

    scheduler.start();

    Workload browser;

    browser.id = 1;
    browser.name = "Browser";
    browser.type =
        WorkloadType::Interactive;

    browser.priority =
        Priority::High;

    browser.cpuDemand = 60.0;
    browser.gpuDemand = 20.0;

    browser.latencyBudgetMs = 8.0;
    browser.interactive = true;

    scheduler.submit(browser);

    Workload graphics;

    graphics.id = 2;
    graphics.name = "3D Rendering";

    graphics.type =
        WorkloadType::Graphics;

    graphics.priority =
        Priority::Normal;

    graphics.cpuDemand = 20.0;
    graphics.gpuDemand = 90.0;

    scheduler.submit(graphics);

    Workload ai;

    ai.id = 3;
    ai.name = "AI Inference";

    ai.type =
        WorkloadType::MachineLearning;

    ai.priority =
        Priority::Normal;

    ai.cpuDemand = 30.0;
    ai.gpuDemand = 60.0;
    ai.npuDemand = 95.0;

    scheduler.submit(ai);

    Workload backup;

    backup.id = 4;
    backup.name = "Background Backup";

    backup.type =
        WorkloadType::Background;

    backup.priority =
        Priority::Background;

    backup.cpuDemand = 40.0;

    backup.preemptible = true;

    scheduler.submit(backup);

    std::cout
        << "Surface Compute Scheduler running.\n"
        << "Press ENTER to stop.\n";

    std::cin.get();

    scheduler.stop();

    return 0;
}
14. tests/SchedulerTests.cpp
#include "SchedulerPolicy.hpp"
#include "WorkloadClassifier.hpp"

#include <cassert>
#include <iostream>

using namespace surface::compute;

int main()
{
    SchedulerPolicy policy;

    /*
        Interactive workload should receive
        high priority / latency treatment.
    */

    HardwareState hardware;

    hardware.cpuUtilization = 30.0;
    hardware.gpuUtilization = 20.0;
    hardware.npuUtilization = 10.0;

    hardware.cpuTemperature = 50.0;
    hardware.gpuTemperature = 50.0;

    hardware.onACPower = true;

    Workload interactive;

    interactive.id = 1;

    interactive.type =
        WorkloadType::Interactive;

    interactive.interactive = true;

    interactive.latencyBudgetMs = 8.0;

    SchedulingDecision decision =
        policy.decide(
            interactive,
            hardware
        );

    assert(
        decision.action ==
        SchedulerAction::Boost
    );

    /*
        Thermal protection.
    */

    hardware.cpuTemperature = 95.0;

    Workload cpuTask;

    cpuTask.id = 2;
    cpuTask.type =
        WorkloadType::General;

    cpuTask.cpuDemand = 80.0;

    decision =
        policy.decide(
            cpuTask,
            hardware
        );

    assert(
        decision.action ==
        SchedulerAction::Throttle
    );

    /*
        GPU workload.
    */

    hardware.cpuTemperature = 50.0;
    hardware.gpuTemperature = 50.0;
    hardware.gpuAvailable = true;

    Workload rendering;

    rendering.id = 3;

    rendering.type =
        WorkloadType::Graphics;

    rendering.cpuDemand = 20.0;
    rendering.gpuDemand = 90.0;

    decision =
        policy.decide(
            rendering,
            hardware
        );

    assert(
        decision.target ==
        ComputeUnit::GPU
    );

    /*
        NPU fallback.
    */

    hardware.npuAvailable = false;

    Workload ai;

    ai.id = 4;

    ai.type =
        WorkloadType::MachineLearning;

    ai.npuDemand = 95.0;
    ai.gpuDemand = 80.0;

    decision =
        policy.decide(
            ai,
            hardware
        );

    assert(
        decision.target ==
        ComputeUnit::GPU
    );

    std::cout
        << "All scheduler tests passed.\n";

    return 0;
}
15. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceComputeScheduler
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceComputeScheduler

    src/main.cpp
    src/WindowsTelemetry.cpp
    src/WorkloadClassifier.cpp
    src/ComputeQueue.cpp
    src/SchedulerPolicy.cpp
    src/ComputeScheduler.cpp
)

target_include_directories(
    SurfaceComputeScheduler
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceComputeScheduler
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    SchedulerTests

    tests/SchedulerTests.cpp
    src/WorkloadClassifier.cpp
    src/ComputeQueue.cpp
    src/SchedulerPolicy.cpp
)

target_include_directories(
    SchedulerTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME SchedulerTests
    COMMAND SchedulerTests
)









Project structure
SurfaceDisplayEngine/
├── CMakeLists.txt
├── include/
│   ├── DisplayTypes.hpp
│   ├── DisplayTelemetry.hpp
│   ├── DisplayPolicy.hpp
│   ├── FramePacer.hpp
│   ├── BrightnessController.hpp
│   ├── RefreshController.hpp
│   ├── DisplayEngine.hpp
│   └── WindowsDisplayBackend.hpp
├── src/
│   ├── DisplayTelemetry.cpp
│   ├── DisplayPolicy.cpp
│   ├── FramePacer.cpp
│   ├── BrightnessController.cpp
│   ├── RefreshController.cpp
│   ├── DisplayEngine.cpp
│   ├── WindowsDisplayBackend.cpp
│   └── main.cpp
└── tests/
    └── DisplayTests.cpp
1. include/DisplayTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace surface::display {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class DisplayMode
{
    PowerSaving,
    Balanced,
    HighRefresh,
    HDR,
    Gaming,
    Video,
    Presentation
};

enum class PixelFormat
{
    SDR,
    HDR10,
    ScRGB
};

enum class RefreshRate
{
    Hz60  = 60,
    Hz90  = 90,
    Hz120 = 120,
    Hz144 = 144
};

struct DisplayState
{
    unsigned width{0};
    unsigned height{0};

    RefreshRate refreshRate{
        RefreshRate::Hz60
    };

    double brightness{50.0};

    double ambientLightLux{0.0};

    double gpuUtilization{0.0};

    double batteryPercent{100.0};

    bool onACPower{true};

    bool hdrSupported{false};
    bool hdrEnabled{false};

    bool vrrSupported{false};
    bool vrrEnabled{false};

    PixelFormat pixelFormat{
        PixelFormat::SDR
    };

    DisplayMode mode{
        DisplayMode::Balanced
    };
};

struct FrameTiming
{
    std::uint64_t frameNumber{0};

    TimePoint submittedAt{
        Clock::now()
    };

    TimePoint presentedAt{
        Clock::now()
    };

    double frameTimeMs{0.0};

    double targetFrameTimeMs{16.666};
};

struct DisplayPolicy
{
    RefreshRate targetRefreshRate{
        RefreshRate::Hz60
    };

    double targetBrightness{50.0};

    bool enableHDR{false};

    bool enableVRR{false};

    double frameRateLimit{60.0};

    double renderScale{1.0};

    bool powerSaving{false};

    std::string reason;
};

struct DisplayConfiguration
{
    double lowBatteryThreshold{25.0};

    double highAmbientLux{500.0};

    double lowAmbientLux{50.0};

    double gpuHighLoad{85.0};

    double brightnessMinimum{5.0};

    double brightnessMaximum{100.0};

    bool allow120Hz{true};
    bool allow144Hz{true};

    bool allowHDR{true};
    bool allowVRR{true};
};

}
2. include/DisplayTelemetry.hpp
#pragma once

#include "DisplayTypes.hpp"

namespace surface::display {

class IDisplayTelemetry
{
public:

    virtual ~IDisplayTelemetry() = default;

    virtual DisplayState read() = 0;
};

}
3. include/WindowsDisplayBackend.hpp
#pragma once

#include "DisplayTelemetry.hpp"

namespace surface::display {

class WindowsDisplayBackend final
    : public IDisplayTelemetry
{
public:

    DisplayState read() override;
};

}
4. src/WindowsDisplayBackend.cpp
#include "WindowsDisplayBackend.hpp"

#ifdef _WIN32

#include <windows.h>

#endif

namespace surface::display {

DisplayState
WindowsDisplayBackend::read()
{
    DisplayState state;

#ifdef _WIN32

    SYSTEM_POWER_STATUS power{};

    if (GetSystemPowerStatus(&power))
    {
        state.batteryPercent =
            static_cast<double>(
                power.BatteryLifePercent
            );

        state.onACPower =
            power.ACLineStatus == 1;
    }

#endif

    /*
        Hardware-specific panel capabilities should
        be populated from supported Windows display
        APIs / DXGI / DisplayConfig.

        Safe defaults are used here rather than
        inventing Surface hardware capabilities.
    */

    state.width = 0;
    state.height = 0;

    state.refreshRate =
        RefreshRate::Hz60;

    state.gpuUtilization = 0.0;

    state.ambientLightLux = 0.0;

    state.hdrSupported = false;
    state.vrrSupported = false;

    return state;
}

}
5. include/DisplayPolicy.hpp
#pragma once

#include "DisplayTypes.hpp"

namespace surface::display {

class DisplayPolicyEngine
{
public:

    explicit DisplayPolicyEngine(
        DisplayConfiguration configuration = {});

    DisplayPolicy calculate(
        const DisplayState& state) const;

private:

    DisplayConfiguration configuration_;

    RefreshRate chooseRefreshRate(
        const DisplayState& state) const;

    double chooseBrightness(
        const DisplayState& state) const;
};

}
6. src/DisplayPolicy.cpp
#include "DisplayPolicy.hpp"

#include <algorithm>

namespace surface::display {

DisplayPolicyEngine::DisplayPolicyEngine(
    DisplayConfiguration configuration)
    : configuration_(configuration)
{
}

RefreshRate
DisplayPolicyEngine::chooseRefreshRate(
    const DisplayState& state) const
{
    /*
        Battery conservation has priority over
        unnecessary high refresh rates.
    */

    if (!state.onACPower &&
        state.batteryPercent <=
            configuration_.lowBatteryThreshold)
    {
        return RefreshRate::Hz60;
    }

    /*
        Gaming / high-GPU workloads can justify
        higher refresh rates.
    */

    if (state.mode == DisplayMode::Gaming)
    {
        if (configuration_.allow144Hz)
        {
            return RefreshRate::Hz144;
        }

        if (configuration_.allow120Hz)
        {
            return RefreshRate::Hz120;
        }
    }

    if (configuration_.allow120Hz &&
        state.gpuUtilization >=
            configuration_.gpuHighLoad)
    {
        return RefreshRate::Hz120;
    }

    /*
        High ambient light can justify a responsive
        display but we avoid automatically forcing
        maximum refresh.
    */

    if (state.mode ==
        DisplayMode::HighRefresh)
    {
        if (configuration_.allow120Hz)
        {
            return RefreshRate::Hz120;
        }
    }

    return RefreshRate::Hz60;
}

double
DisplayPolicyEngine::chooseBrightness(
    const DisplayState& state) const
{
    double brightness = state.brightness;

    /*
        Simple ambient-light policy.
        A production implementation could use a
        calibrated sensor curve rather than these
        broad bands.
    */

    if (state.ambientLightLux >=
        configuration_.highAmbientLux)
    {
        brightness =
            std::max(
                brightness,
                80.0
            );
    }
    else if (
        state.ambientLightLux <=
        configuration_.lowAmbientLux)
    {
        brightness =
            std::min(
                brightness,
                35.0
            );
    }

    return std::clamp(
        brightness,
        configuration_.brightnessMinimum,
        configuration_.brightnessMaximum
    );
}

DisplayPolicy
DisplayPolicyEngine::calculate(
    const DisplayState& state) const
{
    DisplayPolicy policy;

    policy.targetRefreshRate =
        chooseRefreshRate(state);

    policy.targetBrightness =
        chooseBrightness(state);

    /*
        HDR policy.
    */

    if (
        configuration_.allowHDR &&
        state.hdrSupported &&
        (
            state.mode == DisplayMode::HDR ||
            state.mode == DisplayMode::Video
        ))
    {
        policy.enableHDR = true;

        policy.reason +=
            "HDR workload; ";
    }

    /*
        VRR policy.
    */

    if (
        configuration_.allowVRR &&
        state.vrrSupported &&
        (
            state.mode == DisplayMode::Gaming ||
            state.mode == DisplayMode::HighRefresh
        ))
    {
        policy.enableVRR = true;

        policy.reason +=
            "VRR appropriate; ";
    }

    /*
        Battery policy.
    */

    if (
        !state.onACPower &&
        state.batteryPercent <=
            configuration_.lowBatteryThreshold)
    {
        policy.powerSaving = true;

        policy.frameRateLimit = 60.0;

        policy.renderScale = 0.85;

        policy.reason +=
            "battery conservation; ";
    }
    else
    {
        switch (policy.targetRefreshRate)
        {
            case RefreshRate::Hz60:
                policy.frameRateLimit = 60.0;
                break;

            case RefreshRate::Hz90:
                policy.frameRateLimit = 90.0;
                break;

            case RefreshRate::Hz120:
                policy.frameRateLimit = 120.0;
                break;

            case RefreshRate::Hz144:
                policy.frameRateLimit = 144.0;
                break;
        }

        policy.renderScale = 1.0;
    }

    return policy;
}

}
7. include/FramePacer.hpp
#pragma once

#include "DisplayTypes.hpp"

#include <chrono>
#include <thread>

namespace surface::display {

class FramePacer
{
public:

    explicit FramePacer(
        double targetFPS = 60.0);

    void setTargetFPS(
        double fps);

    void beginFrame();

    void endFrame();

    double targetFrameTimeMs() const;

private:

    double targetFPS_;

    TimePoint frameStart_;

    std::chrono::duration<double, std::milli>
        targetDuration_;
};

}
8. src/FramePacer.cpp
#include "FramePacer.hpp"

#include <algorithm>

namespace surface::display {

FramePacer::FramePacer(
    double targetFPS)
    : targetFPS_(
        std::max(1.0, targetFPS))
{
    targetDuration_ =
        std::chrono::duration<double, std::milli>(
            1000.0 / targetFPS_
        );
}

void FramePacer::setTargetFPS(
    double fps)
{
    targetFPS_ =
        std::max(
            1.0,
            fps
        );

    targetDuration_ =
        std::chrono::duration<double, std::milli>(
            1000.0 / targetFPS_
        );
}

void FramePacer::beginFrame()
{
    frameStart_ =
        Clock::now();
}

void FramePacer::endFrame()
{
    const auto now =
        Clock::now();

    const auto elapsed =
        now - frameStart_;

    const auto remaining =
        targetDuration_ -
        std::chrono::duration<double, std::milli>(
            elapsed
        );

    if (remaining.count() > 0.0)
    {
        std::this_thread::sleep_for(
            remaining
        );
    }
}

double FramePacer::targetFrameTimeMs() const
{
    return targetDuration_.count();
}

}
9. include/BrightnessController.hpp
#pragma once

#include "DisplayTypes.hpp"

namespace surface::display {

class BrightnessController
{
public:

    explicit BrightnessController(
        double minimum = 5.0,
        double maximum = 100.0);

    double calculate(
        const DisplayState& state) const;

private:

    double minimum_;
    double maximum_;
};

}
10. src/BrightnessController.cpp
#include "BrightnessController.hpp"

#include <algorithm>

namespace surface::display {

BrightnessController::BrightnessController(
    double minimum,
    double maximum)
    : minimum_(minimum),
      maximum_(maximum)
{
}

double BrightnessController::calculate(
    const DisplayState& state) const
{
    double result =
        state.brightness;

    if (state.ambientLightLux < 20.0)
    {
        result =
            std::min(
                result,
                25.0
            );
    }
    else if (
        state.ambientLightLux > 800.0)
    {
        result =
            std::max(
                result,
                90.0
            );
    }

    return std::clamp(
        result,
        minimum_,
        maximum_
    );
}

}
11. include/RefreshController.hpp
#pragma once

#include "DisplayTypes.hpp"

namespace surface::display {

class RefreshController
{
public:

    void setTarget(
        RefreshRate rate);

    RefreshRate target() const;

    double targetFPS() const;

private:

    RefreshRate targetRate_{
        RefreshRate::Hz60
    };
};

}
12. src/RefreshController.cpp
#include "RefreshController.hpp"

namespace surface::display {

void RefreshController::setTarget(
    RefreshRate rate)
{
    targetRate_ = rate;
}

RefreshRate RefreshController::target() const
{
    return targetRate_;
}

double RefreshController::targetFPS() const
{
    return static_cast<double>(
        static_cast<int>(
            targetRate_
        )
    );
}

}
13. include/DisplayEngine.hpp
#pragma once

#include "BrightnessController.hpp"
#include "DisplayPolicy.hpp"
#include "DisplayTelemetry.hpp"
#include "FramePacer.hpp"
#include "RefreshController.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

namespace surface::display {

class DisplayEngine
{
public:

    using PolicyCallback =
        std::function<void(
            const DisplayPolicy&)>;

    explicit DisplayEngine(
        std::unique_ptr<IDisplayTelemetry>
            telemetry,
        DisplayConfiguration configuration = {});

    ~DisplayEngine();

    void start();

    void stop();

    void update();

    DisplayState state() const;

    DisplayPolicy policy() const;

    FramePacer& framePacer();

    void setPolicyCallback(
        PolicyCallback callback);

private:

    void loop();

    std::unique_ptr<
        IDisplayTelemetry
    > telemetry_;

    DisplayPolicyEngine policyEngine_;

    BrightnessController
        brightnessController_;

    RefreshController
        refreshController_;

    FramePacer
        framePacer_;

    mutable std::mutex mutex_;

    DisplayState latestState_;
    DisplayPolicy latestPolicy_;

    PolicyCallback callback_;

    std::atomic<bool> running_{false};

    std::thread worker_;
};

}
14. src/DisplayEngine.cpp
#include "DisplayEngine.hpp"

#include <chrono>
#include <iostream>

namespace surface::display {

DisplayEngine::DisplayEngine(
    std::unique_ptr<IDisplayTelemetry>
        telemetry,
    DisplayConfiguration configuration)
    : telemetry_(std::move(telemetry)),
      policyEngine_(configuration),
      brightnessController_(
          configuration.brightnessMinimum,
          configuration.brightnessMaximum),
      framePacer_(60.0)
{
}

DisplayEngine::~DisplayEngine()
{
    stop();
}

void DisplayEngine::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    worker_ =
        std::thread(
            &DisplayEngine::loop,
            this
        );
}

void DisplayEngine::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }
}

void DisplayEngine::update()
{
    if (!telemetry_)
        return;

    DisplayState state =
        telemetry_->read();

    DisplayPolicy policy =
        policyEngine_.calculate(
            state
        );

    policy.targetBrightness =
        brightnessController_.calculate(
            state
        );

    {
        std::lock_guard lock(mutex_);

        latestState_ =
            state;

        latestPolicy_ =
            policy;

        refreshController_.setTarget(
            policy.targetRefreshRate
        );

        framePacer_.setTargetFPS(
            policy.frameRateLimit
        );
    }

    PolicyCallback callback;

    {
        std::lock_guard lock(mutex_);

        callback = callback_;
    }

    if (callback)
    {
        callback(policy);
    }
}

void DisplayEngine::loop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        update();

        std::this_thread::sleep_for(
            500ms
        );
    }
}

DisplayState DisplayEngine::state() const
{
    std::lock_guard lock(mutex_);

    return latestState_;
}

DisplayPolicy DisplayEngine::policy() const
{
    std::lock_guard lock(mutex_);

    return latestPolicy_;
}

FramePacer& DisplayEngine::framePacer()
{
    return framePacer_;
}

void DisplayEngine::setPolicyCallback(
    PolicyCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}

}
15. src/main.cpp
#include "DisplayEngine.hpp"
#include "WindowsDisplayBackend.hpp"

#include <iostream>
#include <memory>

using namespace surface::display;

int main()
{
    DisplayConfiguration configuration;

    configuration.allow120Hz = true;
    configuration.allow144Hz = true;

    configuration.allowHDR = true;
    configuration.allowVRR = true;

    auto telemetry =
        std::make_unique<
            WindowsDisplayBackend
        >();

    DisplayEngine engine(
        std::move(telemetry),
        configuration
    );

    engine.setPolicyCallback(
        [](const DisplayPolicy& policy)
        {
            std::cout
                << "\n[DISPLAY POLICY]\n"
                << "Refresh: "
                << static_cast<int>(
                    policy.targetRefreshRate
                )
                << " Hz\n"
                << "Brightness: "
                << policy.targetBrightness
                << "%\n"
                << "HDR: "
                << (policy.enableHDR
                    ? "ON"
                    : "OFF")
                << "\n"
                << "VRR: "
                << (policy.enableVRR
                    ? "ON"
                    : "OFF")
                << "\n"
                << "Frame limit: "
                << policy.frameRateLimit
                << " FPS\n"
                << "Render scale: "
                << policy.renderScale
                << "\n"
                << "Power saving: "
                << (policy.powerSaving
                    ? "ON"
                    : "OFF")
                << "\n"
                << "Reason: "
                << policy.reason
                << "\n";
        }
    );

    engine.start();

    std::cout
        << "====================================\n"
        << " Surface Display Engine\n"
        << " C++20 / Windows\n"
        << "====================================\n";

    std::cout
        << "Display policy engine running.\n";

    std::cout
        << "Press ENTER to exit.\n";

    std::cin.get();

    engine.stop();

    return 0;
}
16. tests/DisplayTests.cpp
#include "DisplayPolicy.hpp"
#include "FramePacer.hpp"
#include "BrightnessController.hpp"

#include <cassert>
#include <iostream>

using namespace surface::display;

int main()
{
    DisplayConfiguration configuration;

    configuration.lowBatteryThreshold =
        25.0;

    configuration.allow120Hz = true;
    configuration.allow144Hz = true;

    DisplayPolicyEngine policyEngine(
        configuration
    );

    /*
        Normal desktop.
    */

    DisplayState normal;

    normal.batteryPercent = 80.0;
    normal.onACPower = true;

    normal.gpuUtilization = 30.0;

    normal.mode =
        DisplayMode::Balanced;

    DisplayPolicy policy =
        policyEngine.calculate(
            normal
        );

    assert(
        policy.targetRefreshRate ==
        RefreshRate::Hz60
    );

    /*
        High refresh mode.
    */

    DisplayState highRefresh =
        normal;

    highRefresh.mode =
        DisplayMode::HighRefresh;

    policy =
        policyEngine.calculate(
            highRefresh
        );

    assert(
        static_cast<int>(
            policy.targetRefreshRate
        ) >= 120
    );

    /*
        Low battery.
    */

    DisplayState battery =
        normal;

    battery.batteryPercent = 10.0;
    battery.onACPower = false;

    policy =
        policyEngine.calculate(
            battery
        );

    assert(
        policy.powerSaving
    );

    assert(
        policy.frameRateLimit == 60.0
    );

    /*
        HDR.
    */

    DisplayState hdr =
        normal;

    hdr.hdrSupported = true;
    hdr.mode =
        DisplayMode::HDR;

    policy =
        policyEngine.calculate(
            hdr
        );

    assert(
        policy.enableHDR
    );

    /*
        Brightness.
    */

    BrightnessController
        brightness(
            5.0,
            100.0
        );

    DisplayState darkRoom =
        normal;

    darkRoom.brightness = 70.0;
    darkRoom.ambientLightLux = 5.0;

    double result =
        brightness.calculate(
            darkRoom
        );

    assert(result <= 25.0);

    /*
        Frame pacing.
    */

    FramePacer pacer(60.0);

    assert(
        pacer.targetFrameTimeMs() > 16.0
    );

    assert(
        pacer.targetFrameTimeMs() < 17.0
    );

    std::cout
        << "All display tests passed.\n";

    return 0;
}
17. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceDisplayEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceDisplayEngine

    src/main.cpp
    src/WindowsDisplayBackend.cpp
    src/DisplayPolicy.cpp
    src/FramePacer.cpp
    src/BrightnessController.cpp
    src/RefreshController.cpp
    src/DisplayEngine.cpp
)

target_include_directories(
    SurfaceDisplayEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceDisplayEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    DisplayTests

    tests/DisplayTests.cpp
    src/DisplayPolicy.cpp
    src/FramePacer.cpp
    src/BrightnessController.cpp
)

target_include_directories(
    DisplayTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME DisplayTests
    COMMAND DisplayTests
)











Project structure
SurfaceConnectivityEngine/
├── CMakeLists.txt
├── include/
│   ├── ConnectivityTypes.hpp
│   ├── NetworkTelemetry.hpp
│   ├── WiFiManager.hpp
│   ├── BluetoothManager.hpp
│   ├── NetworkQuality.hpp
│   ├── ConnectivityPolicy.hpp
│   ├── ConnectivityEngine.hpp
│   └── WindowsNetworkBackend.hpp
├── src/
│   ├── NetworkTelemetry.cpp
│   ├── WiFiManager.cpp
│   ├── BluetoothManager.cpp
│   ├── NetworkQuality.cpp
│   ├── ConnectivityPolicy.cpp
│   ├── ConnectivityEngine.cpp
│   ├── WindowsNetworkBackend.cpp
│   └── main.cpp
└── tests/
    └── ConnectivityTests.cpp
1. include/ConnectivityTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace surface::connectivity {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class InterfaceType
{
    WiFi,
    Ethernet,
    Bluetooth,
    Cellular,
    Unknown
};

enum class ConnectionState
{
    Disconnected,
    Connecting,
    Connected,
    Limited
};

enum class NetworkQuality
{
    Unknown,
    Poor,
    Fair,
    Good,
    Excellent
};

enum class PowerMode
{
    Performance,
    Balanced,
    BatterySaver
};

struct NetworkInterface
{
    std::string name;

    InterfaceType type{
        InterfaceType::Unknown
    };

    ConnectionState state{
        ConnectionState::Disconnected
    };

    std::uint64_t rxBytes{0};
    std::uint64_t txBytes{0};

    double linkSpeedMbps{0.0};

    int signalStrength{0};

    bool metered{false};
};

struct WiFiNetwork
{
    std::string ssid;

    int signalStrength{0};

    int channel{0};

    double frequencyMHz{0.0};

    bool secured{true};

    bool currentlyConnected{false};
};

struct BluetoothDevice
{
    std::string name;

    std::string address;

    bool connected{false};

    int signalStrength{0};
};

struct NetworkMetrics
{
    double latencyMs{0.0};

    double jitterMs{0.0};

    double packetLossPercent{0.0};

    double downloadMbps{0.0};

    double uploadMbps{0.0};

    NetworkQuality quality{
        NetworkQuality::Unknown
    };
};

struct ConnectivityState
{
    std::vector<NetworkInterface>
        interfaces;

    std::vector<WiFiNetwork>
        wifiNetworks;

    std::vector<BluetoothDevice>
        bluetoothDevices;

    NetworkMetrics metrics;

    bool internetAvailable{false};

    bool onACPower{true};

    double batteryPercent{100.0};
};

struct ConnectivityPolicy
{
    bool keepWiFiAwake{true};

    bool allowBackgroundNetworking{true};

    bool preferLowPowerMode{false};

    bool enableBluetoothScan{false};

    bool preferHighQualityWiFi{true};

    bool avoidMeteredConnection{true};

    int minimumWiFiSignal{20};

    double maxBackgroundMbps{100.0};

    std::string reason;
};

struct ConnectivityConfiguration
{
    int poorSignalThreshold{20};

    int goodSignalThreshold{60};

    double poorLatencyMs{150.0};

    double goodLatencyMs{50.0};

    double poorPacketLoss{5.0};

    double goodPacketLoss{1.0};

    double lowBatteryThreshold{20.0};

    bool allowBackgroundOnBattery{true};

    bool avoidMeteredBackgroundTraffic{true};
};

}
2. include/NetworkTelemetry.hpp
#pragma once

#include "ConnectivityTypes.hpp"

namespace surface::connectivity {

class INetworkTelemetry
{
public:

    virtual ~INetworkTelemetry() = default;

    virtual ConnectivityState read() = 0;
};

}
3. include/WiFiManager.hpp
#pragma once

#include "ConnectivityTypes.hpp"

#include <vector>

namespace surface::connectivity {

class WiFiManager
{
public:

    static const WiFiNetwork*
    bestNetwork(
        const std::vector<WiFiNetwork>& networks,
        int minimumSignal);

    static bool usable(
        const WiFiNetwork& network,
        int minimumSignal);
};

}
4. src/WiFiManager.cpp
#include "WiFiManager.hpp"

#include <algorithm>

namespace surface::connectivity {

const WiFiNetwork*
WiFiManager::bestNetwork(
    const std::vector<WiFiNetwork>& networks,
    int minimumSignal)
{
    const WiFiNetwork* best = nullptr;

    for (const auto& network : networks)
    {
        if (!usable(
                network,
                minimumSignal))
        {
            continue;
        }

        if (!best ||
            network.signalStrength >
            best->signalStrength)
        {
            best = &network;
        }
    }

    return best;
}

bool WiFiManager::usable(
    const WiFiNetwork& network,
    int minimumSignal)
{
    return
        network.signalStrength >=
        minimumSignal;
}

}
5. include/BluetoothManager.hpp
#pragma once

#include "ConnectivityTypes.hpp"

#include <vector>

namespace surface::connectivity {

class BluetoothManager
{
public:

    static unsigned connectedCount(
        const std::vector<BluetoothDevice>& devices);

    static bool hasConnectedAudioDevice(
        const std::vector<BluetoothDevice>& devices);
};

}
6. src/BluetoothManager.cpp
#include "BluetoothManager.hpp"

namespace surface::connectivity {

unsigned BluetoothManager::connectedCount(
    const std::vector<BluetoothDevice>& devices)
{
    unsigned count = 0;

    for (const auto& device : devices)
    {
        if (device.connected)
        {
            ++count;
        }
    }

    return count;
}

bool BluetoothManager::hasConnectedAudioDevice(
    const std::vector<BluetoothDevice>& devices)
{
    /*
        Device-class information should be populated
        by the Windows Bluetooth layer in production.

        This generic policy layer only knows whether
        a Bluetooth device is connected.
    */

    for (const auto& device : devices)
    {
        if (device.connected)
        {
            return true;
        }
    }

    return false;
}

}
7. include/NetworkQuality.hpp
#pragma once

#include "ConnectivityTypes.hpp"

namespace surface::connectivity {

class NetworkQualityAnalyzer
{
public:

    NetworkQuality classify(
        const NetworkMetrics& metrics) const;

    double score(
        const NetworkMetrics& metrics) const;
};

}
8. src/NetworkQuality.cpp
#include "NetworkQuality.hpp"

#include <algorithm>

namespace surface::connectivity {

NetworkQuality
NetworkQualityAnalyzer::classify(
    const NetworkMetrics& metrics) const
{
    const double value =
        score(metrics);

    if (value >= 90.0)
    {
        return NetworkQuality::Excellent;
    }

    if (value >= 70.0)
    {
        return NetworkQuality::Good;
    }

    if (value >= 45.0)
    {
        return NetworkQuality::Fair;
    }

    if (value > 0.0)
    {
        return NetworkQuality::Poor;
    }

    return NetworkQuality::Unknown;
}

double
NetworkQualityAnalyzer::score(
    const NetworkMetrics& metrics) const
{
    /*
        This is intentionally a generic scoring model.
        Production networking can replace this with
        application-specific QoS measurements.
    */

    double latencyScore =
        100.0 -
        std::min(
            metrics.latencyMs / 2.0,
            100.0
        );

    double lossScore =
        100.0 -
        std::min(
            metrics.packetLossPercent * 10.0,
            100.0
        );

    double jitterScore =
        100.0 -
        std::min(
            metrics.jitterMs * 2.0,
            100.0
        );

    double throughputScore =
        std::min(
            metrics.downloadMbps / 5.0,
            100.0
        );

    return
        latencyScore * 0.35 +
        lossScore * 0.30 +
        jitterScore * 0.15 +
        throughputScore * 0.20;
}

}
9. include/ConnectivityPolicy.hpp
#pragma once

#include "ConnectivityTypes.hpp"

namespace surface::connectivity {

class ConnectivityPolicyEngine
{
public:

    explicit ConnectivityPolicyEngine(
        ConnectivityConfiguration configuration = {});

    ConnectivityPolicy calculate(
        const ConnectivityState& state) const;

private:

    ConnectivityConfiguration
        configuration_;
};

}
10. src/ConnectivityPolicy.cpp
#include "ConnectivityPolicy.hpp"

namespace surface::connectivity {

ConnectivityPolicyEngine::ConnectivityPolicyEngine(
    ConnectivityConfiguration configuration)
    : configuration_(configuration)
{
}

ConnectivityPolicy
ConnectivityPolicyEngine::calculate(
    const ConnectivityState& state) const
{
    ConnectivityPolicy policy;

    /*
        Start with normal connected behaviour.
    */

    policy.keepWiFiAwake =
        state.internetAvailable;

    policy.allowBackgroundNetworking = true;

    policy.preferHighQualityWiFi = true;

    /*
        Metered networks.
    */

    bool meteredConnection = false;

    for (const auto& interface :
         state.interfaces)
    {
        if (
            interface.state ==
                ConnectionState::Connected &&
            interface.metered)
        {
            meteredConnection = true;
            break;
        }
    }

    if (
        meteredConnection &&
        configuration_.avoidMeteredBackgroundTraffic)
    {
        policy.avoidMeteredConnection = true;

        policy.allowBackgroundNetworking = false;

        policy.reason +=
            "Metered network; ";
    }

    /*
        Poor network quality.
    */

    if (
        state.metrics.latencyMs >
            configuration_.poorLatencyMs ||
        state.metrics.packetLossPercent >
            configuration_.poorPacketLoss)
    {
        policy.reason +=
            "Poor network quality; ";

        policy.minimumWiFiSignal =
            configuration_.goodSignalThreshold;
    }

    /*
        Battery policy.
    */

    if (
        !state.onACPower &&
        state.batteryPercent <=
            configuration_.lowBatteryThreshold)
    {
        policy.preferLowPowerMode = true;

        policy.keepWiFiAwake =
            state.internetAvailable;

        if (!configuration_.allowBackgroundOnBattery)
        {
            policy.allowBackgroundNetworking =
                false;
        }

        policy.reason +=
            "Battery conservation; ";
    }

    /*
        If no internet exists, don't waste power
        aggressively maintaining background network
        activity.
    */

    if (!state.internetAvailable)
    {
        policy.allowBackgroundNetworking =
            false;

        policy.keepWiFiAwake = false;

        policy.reason +=
            "No internet connection; ";
    }

    /*
        Bluetooth scanning is expensive compared
        with simply maintaining known connections.
    */

    policy.enableBluetoothScan = false;

    if (
        state.internetAvailable &&
        state.batteryPercent > 50.0)
    {
        policy.enableBluetoothScan = true;
    }

    return policy;
}

}
11. include/ConnectivityEngine.hpp
#pragma once

#include "ConnectivityTypes.hpp"
#include "NetworkTelemetry.hpp"
#include "ConnectivityPolicy.hpp"
#include "NetworkQuality.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace surface::connectivity {

class ConnectivityEngine
{
public:

    using PolicyCallback =
        std::function<void(
            const ConnectivityPolicy&)>;

    explicit ConnectivityEngine(
        std::unique_ptr<INetworkTelemetry>
            telemetry,
        ConnectivityConfiguration
            configuration = {});

    ~ConnectivityEngine();

    void start();

    void stop();

    void update();

    ConnectivityState state() const;

    ConnectivityPolicy policy() const;

    void setPolicyCallback(
        PolicyCallback callback);

private:

    void loop();

    std::unique_ptr<
        INetworkTelemetry
    > telemetry_;

    ConnectivityPolicyEngine
        policyEngine_;

    NetworkQualityAnalyzer
        qualityAnalyzer_;

    mutable std::mutex mutex_;

    ConnectivityState latestState_;

    ConnectivityPolicy latestPolicy_;

    PolicyCallback callback_;

    std::atomic<bool>
        running_{false};

    std::thread worker_;
};

}
12. src/ConnectivityEngine.cpp
#include "ConnectivityEngine.hpp"

#include <chrono>

namespace surface::connectivity {

ConnectivityEngine::ConnectivityEngine(
    std::unique_ptr<INetworkTelemetry>
        telemetry,
    ConnectivityConfiguration configuration)
    : telemetry_(std::move(telemetry)),
      policyEngine_(configuration)
{
}

ConnectivityEngine::~ConnectivityEngine()
{
    stop();
}

void ConnectivityEngine::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    worker_ =
        std::thread(
            &ConnectivityEngine::loop,
            this
        );
}

void ConnectivityEngine::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }
}

void ConnectivityEngine::update()
{
    if (!telemetry_)
    {
        return;
    }

    ConnectivityState state =
        telemetry_->read();

    state.metrics.quality =
        qualityAnalyzer_.classify(
            state.metrics
        );

    ConnectivityPolicy policy =
        policyEngine_.calculate(
            state
        );

    PolicyCallback callback;

    {
        std::lock_guard lock(mutex_);

        latestState_ =
            state;

        latestPolicy_ =
            policy;

        callback =
            callback_;
    }

    if (callback)
    {
        callback(policy);
    }
}

void ConnectivityEngine::loop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        update();

        std::this_thread::sleep_for(
            1000ms
        );
    }
}

ConnectivityState
ConnectivityEngine::state() const
{
    std::lock_guard lock(mutex_);

    return latestState_;
}

ConnectivityPolicy
ConnectivityEngine::policy() const
{
    std::lock_guard lock(mutex_);

    return latestPolicy_;
}

void ConnectivityEngine::setPolicyCallback(
    PolicyCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}

}
13. src/NetworkTelemetry.cpp
#include "NetworkTelemetry.hpp"

namespace surface::connectivity {

/*
    The concrete Windows implementation is kept in
    WindowsNetworkBackend.cpp.

    This translation unit is intentionally minimal so
    the telemetry interface remains platform-neutral.
*/

}
14. include/WindowsNetworkBackend.hpp
#pragma once

#include "NetworkTelemetry.hpp"

namespace surface::connectivity {

class WindowsNetworkBackend final
    : public INetworkTelemetry
{
public:

    ConnectivityState read() override;
};

}
15. src/WindowsNetworkBackend.cpp
#include "WindowsNetworkBackend.hpp"

#ifdef _WIN32

#include <windows.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

#endif

namespace surface::connectivity {

ConnectivityState
WindowsNetworkBackend::read()
{
    ConnectivityState state;

#ifdef _WIN32

    /*
        Basic system power state.
    */

    SYSTEM_POWER_STATUS power{};

    if (GetSystemPowerStatus(&power))
    {
        state.batteryPercent =
            static_cast<double>(
                power.BatteryLifePercent
            );

        state.onACPower =
            power.ACLineStatus == 1;
    }

    /*
        Windows IP Helper API.

        This gives us a safe starting point for
        discovering network adapters.

        A production implementation should additionally
        use the appropriate Windows WLAN / Network List
        Manager / modern networking APIs for detailed
        Wi-Fi state.
    */

    ULONG bufferSize = 0;

    DWORD result =
        GetAdaptersAddresses(
            AF_UNSPEC,
            0,
            nullptr,
            nullptr,
            &bufferSize
        );

    if (
        result == ERROR_BUFFER_OVERFLOW &&
        bufferSize > 0)
    {
        std::vector<std::uint8_t>
            buffer(bufferSize);

        auto* adapters =
            reinterpret_cast<
                IP_ADAPTER_ADDRESSES*
            >(buffer.data());

        result =
            GetAdaptersAddresses(
                AF_UNSPEC,
                0,
                nullptr,
                adapters,
                &bufferSize
            );

        if (result == NO_ERROR)
        {
            for (
                auto* adapter = adapters;
                adapter != nullptr;
                adapter = adapter->Next)
            {
                NetworkInterface interface;

                if (adapter->FriendlyName)
                {
                    /*
                        Full Unicode conversion is omitted
                        here for brevity of the platform
                        adapter layer.
                    */
                }

                if (
                    adapter->IfType ==
                    IF_TYPE_IEEE80211)
                {
                    interface.type =
                        InterfaceType::WiFi;
                }
                else if (
                    adapter->IfType ==
                    IF_TYPE_ETHERNET_CSMACD)
                {
                    interface.type =
                        InterfaceType::Ethernet;
                }
                else
                {
                    interface.type =
                        InterfaceType::Unknown;
                }

                interface.state =
                    adapter->OperStatus ==
                        IfOperStatusUp
                        ? ConnectionState::Connected
                        : ConnectionState::Disconnected;

                interface.linkSpeedMbps =
                    static_cast<double>(
                        adapter->TransmitLinkSpeed
                    ) /
                    1'000'000.0;

                state.interfaces.push_back(
                    interface
                );

                if (
                    interface.state ==
                    ConnectionState::Connected)
                {
                    state.internetAvailable =
                        true;
                }
            }
        }
    }

#endif

    /*
        Detailed Wi-Fi signal, SSID, Bluetooth devices,
        latency, packet loss and throughput should be
        populated through their respective supported
        Windows APIs.

        No fake measurements are inserted here.
    */

    return state;
}

}
16. src/main.cpp
#include "ConnectivityEngine.hpp"
#include "WindowsNetworkBackend.hpp"

#include <iostream>
#include <memory>

using namespace surface::connectivity;

int main()
{
    ConnectivityConfiguration configuration;

    configuration.lowBatteryThreshold =
        20.0;

    configuration.allowBackgroundOnBattery =
        true;

    configuration.avoidMeteredBackgroundTraffic =
        true;

    auto telemetry =
        std::make_unique<
            WindowsNetworkBackend
        >();

    ConnectivityEngine engine(
        std::move(telemetry),
        configuration
    );

    engine.setPolicyCallback(
        [](const ConnectivityPolicy& policy)
        {
            std::cout
                << "\n================================\n"
                << " CONNECTIVITY POLICY\n"
                << "================================\n"

                << "Wi-Fi awake: "
                << (
                    policy.keepWiFiAwake
                    ? "YES"
                    : "NO"
                )

                << "\nBackground networking: "
                << (
                    policy.allowBackgroundNetworking
                    ? "YES"
                    : "NO"
                )

                << "\nBluetooth scanning: "
                << (
                    policy.enableBluetoothScan
                    ? "YES"
                    : "NO"
                )

                << "\nLow-power mode: "
                << (
                    policy.preferLowPowerMode
                    ? "YES"
                    : "NO"
                )

                << "\nMinimum Wi-Fi signal: "
                << policy.minimumWiFiSignal

                << "\nReason: "
                << policy.reason

                << "\n";
        }
    );

    engine.start();

    std::cout
        << "====================================\n"
        << " Surface Connectivity Engine\n"
        << " C++20 / Windows\n"
        << "====================================\n";

    std::cout
        << "Wi-Fi/Bluetooth/network policy engine running.\n";

    std::cout
        << "Press ENTER to stop.\n";

    std::cin.get();

    engine.stop();

    return 0;
}
17. tests/ConnectivityTests.cpp
#include "NetworkQuality.hpp"
#include "WiFiManager.hpp"
#include "BluetoothManager.hpp"
#include "ConnectivityPolicy.hpp"

#include <cassert>
#include <iostream>

using namespace surface::connectivity;

int main()
{
    /*
        Network quality.
    */

    NetworkQualityAnalyzer analyzer;

    NetworkMetrics excellent;

    excellent.latencyMs = 10.0;
    excellent.jitterMs = 2.0;
    excellent.packetLossPercent = 0.0;
    excellent.downloadMbps = 500.0;

    const auto excellentQuality =
        analyzer.classify(excellent);

    assert(
        excellentQuality ==
        NetworkQuality::Excellent
    );

    /*
        Poor connection.
    */

    NetworkMetrics poor;

    poor.latencyMs = 300.0;
    poor.jitterMs = 80.0;
    poor.packetLossPercent = 10.0;
    poor.downloadMbps = 1.0;

    const auto poorQuality =
        analyzer.classify(poor);

    assert(
        poorQuality ==
        NetworkQuality::Poor
    );

    /*
        Wi-Fi selection.
    */

    std::vector<WiFiNetwork>
        networks;

    networks.push_back(
        {
            "Network A",
            30,
            6,
            2437.0,
            true,
            false
        }
    );

    networks.push_back(
        {
            "Network B",
            85,
            36,
            5180.0,
            true,
            false
        }
    );

    networks.push_back(
        {
            "Network C",
            10,
            1,
            2412.0,
            true,
            false
        }
    );

    const WiFiNetwork* best =
        WiFiManager::bestNetwork(
            networks,
            20
        );

    assert(best != nullptr);

    assert(
        best->ssid ==
        "Network B"
    );

    /*
        Bluetooth.
    */

    std::vector<BluetoothDevice>
        devices;

    devices.push_back(
        {
            "Keyboard",
            "00:11:22:33:44:55",
            true,
            70
        }
    );

    devices.push_back(
        {
            "Mouse",
            "AA:BB:CC:DD:EE:FF",
            false,
            0
        }
    );

    assert(
        BluetoothManager::connectedCount(
            devices
        ) == 1
    );

    /*
        Battery policy.
    */

    ConnectivityConfiguration
        configuration;

    ConnectivityPolicyEngine
        policyEngine(configuration);

    ConnectivityState batteryState;

    batteryState.internetAvailable =
        true;

    batteryState.onACPower =
        false;

    batteryState.batteryPercent =
        10.0;

    ConnectivityPolicy policy =
        policyEngine.calculate(
            batteryState
        );

    assert(
        policy.preferLowPowerMode
    );

    /*
        No network.
    */

    ConnectivityState offline;

    offline.internetAvailable =
        false;

    policy =
        policyEngine.calculate(
            offline
        );

    assert(
        !policy.keepWiFiAwake
    );

    assert(
        !policy.allowBackgroundNetworking
    );

    std::cout
        << "All connectivity tests passed.\n";

    return 0;
}
18. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceConnectivityEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceConnectivityEngine

    src/main.cpp
    src/NetworkTelemetry.cpp
    src/WindowsNetworkBackend.cpp
    src/WiFiManager.cpp
    src/BluetoothManager.cpp
    src/NetworkQuality.cpp
    src/ConnectivityPolicy.cpp
    src/ConnectivityEngine.cpp
)

target_include_directories(
    SurfaceConnectivityEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceConnectivityEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    ConnectivityTests

    tests/ConnectivityTests.cpp
    src/WiFiManager.cpp
    src/BluetoothManager.cpp
    src/NetworkQuality.cpp
    src/ConnectivityPolicy.cpp
)

target_include_directories(
    ConnectivityTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME ConnectivityTests
    COMMAND ConnectivityTests
)








Project structure
SurfaceCameraEngine/
├── CMakeLists.txt
├── include/
│   ├── CameraTypes.hpp
│   ├── CameraFrame.hpp
│   ├── CameraDevice.hpp
│   ├── SensorFusion.hpp
│   ├── ExposureController.hpp
│   ├── ImagePipeline.hpp
│   ├── CameraPolicy.hpp
│   ├── CameraEngine.hpp
│   └── WindowsCameraBackend.hpp
├── src/
│   ├── CameraDevice.cpp
│   ├── SensorFusion.cpp
│   ├── ExposureController.cpp
│   ├── ImagePipeline.cpp
│   ├── CameraPolicy.cpp
│   ├── CameraEngine.cpp
│   ├── WindowsCameraBackend.cpp
│   └── main.cpp
└── tests/
    └── CameraTests.cpp
1. include/CameraTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace surface::camera {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class CameraType
{
    Front,
    Rear,
    Depth,
    IR,
    Unknown
};

enum class CameraMode
{
    Photo,
    Video,
    Portrait,
    LowLight,
    Document,
    VideoConference
};

enum class ExposureMode
{
    Auto,
    Manual,
    Locked
};

enum class FocusMode
{
    Auto,
    Continuous,
    Manual,
    Locked
};

enum class WhiteBalanceMode
{
    Auto,
    Daylight,
    Cloudy,
    Tungsten,
    Fluorescent,
    Manual
};

enum class PrivacyState
{
    CameraAvailable,
    CameraInUse,
    CameraBlocked
};

enum class PixelFormat
{
    Unknown,
    NV12,
    YUY2,
    RGB24,
    BGRA32,
    Gray8
};

struct CameraResolution
{
    std::uint32_t width{0};
    std::uint32_t height{0};

    bool operator==(const CameraResolution&) const = default;
};

struct ExposureSettings
{
    ExposureMode mode{
        ExposureMode::Auto
    };

    double exposureTimeMs{8.0};

    double gain{1.0};

    double compensation{0.0};
};

struct FocusSettings
{
    FocusMode mode{
        FocusMode::Continuous
    };

    double focusDistance{0.0};

    double focusPosition{0.5};
};

struct WhiteBalanceSettings
{
    WhiteBalanceMode mode{
        WhiteBalanceMode::Auto
    };

    double temperatureK{5500.0};

    double tint{0.0};
};

struct CameraCapabilities
{
    CameraResolution maxResolution{
        1920,
        1080
    };

    std::vector<CameraResolution>
        resolutions;

    std::vector<double>
        frameRates;

    bool autofocus{true};

    bool autoExposure{true};

    bool autoWhiteBalance{true};

    bool hdr{false};

    bool hardwareVideoProcessing{false};

    bool depthSensor{false};

    bool irSensor{false};
};

struct AmbientSensorData
{
    double illuminanceLux{0.0};

    double colorTemperatureK{0.0};

    bool valid{false};
};

struct MotionSensorData
{
    double accelerationX{0.0};
    double accelerationY{0.0};
    double accelerationZ{0.0};

    double gyroX{0.0};
    double gyroY{0.0};
    double gyroZ{0.0};

    bool valid{false};
};

struct CameraState
{
    CameraType type{
        CameraType::Unknown
    };

    CameraMode mode{
        CameraMode::Photo
    };

    CameraResolution resolution{
        1920,
        1080
    };

    double frameRate{30.0};

    PixelFormat format{
        PixelFormat::NV12
    };

    ExposureSettings exposure;

    FocusSettings focus;

    WhiteBalanceSettings whiteBalance;

    AmbientSensorData ambient;

    MotionSensorData motion;

    PrivacyState privacy{
        PrivacyState::CameraAvailable
    };

    bool hdrEnabled{false};

    bool stabilizationEnabled{true};

    bool hardwareAcceleration{true};
};

struct CameraPolicy
{
    ExposureSettings exposure;

    FocusSettings focus;

    WhiteBalanceSettings whiteBalance;

    double targetFrameRate{30.0};

    CameraResolution targetResolution{
        1920,
        1080
    };

    bool hdrEnabled{false};

    bool stabilizationEnabled{true};

    bool lowLightEnhancement{false};

    bool noiseReduction{false};

    bool hardwareProcessing{true};

    std::string reason;
};

}
2. include/CameraFrame.hpp
#pragma once

#include "CameraTypes.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace surface::camera {

struct CameraFrame
{
    CameraResolution resolution;

    PixelFormat format{
        PixelFormat::Unknown
    };

    TimePoint timestamp{
        Clock::now()
    };

    std::uint64_t frameNumber{0};

    std::vector<std::uint8_t> data;

    double exposureTimeMs{0.0};

    double gain{1.0};

    double focusPosition{0.0};

    bool keyFrame{false};

    bool hdr{false};
};

using CameraFramePtr =
    std::shared_ptr<CameraFrame>;

}
3. include/CameraDevice.hpp
#pragma once

#include "CameraFrame.hpp"
#include "CameraTypes.hpp"

#include <memory>

namespace surface::camera {

class ICameraDevice
{
public:

    virtual ~ICameraDevice() = default;

    virtual bool open() = 0;

    virtual void close() = 0;

    virtual bool isOpen() const = 0;

    virtual CameraCapabilities
    capabilities() const = 0;

    virtual bool configure(
        const CameraState& state) = 0;

    virtual CameraFramePtr
    captureFrame() = 0;
};

}
4. include/SensorFusion.hpp
#pragma once

#include "CameraTypes.hpp"

namespace surface::camera {

struct StabilizationResult
{
    double translationX{0.0};

    double translationY{0.0};

    double rotation{0.0};

    double confidence{0.0};
};

class SensorFusion
{
public:

    StabilizationResult estimate(
        const MotionSensorData& motion,
        double deltaTimeSeconds) const;

private:

    static double clamp(
        double value,
        double minimum,
        double maximum);
};

}
5. src/SensorFusion.cpp
#include "SensorFusion.hpp"

#include <algorithm>
#include <cmath>

namespace surface::camera {

double SensorFusion::clamp(
    double value,
    double minimum,
    double maximum)
{
    return std::max(
        minimum,
        std::min(
            maximum,
            value
        )
    );
}

StabilizationResult
SensorFusion::estimate(
    const MotionSensorData& motion,
    double deltaTimeSeconds) const
{
    StabilizationResult result;

    if (!motion.valid)
    {
        return result;
    }

    /*
        Simple inertial estimate.

        A production implementation can replace this
        with a Kalman filter / complementary filter
        using actual IMU timestamps and camera motion.
    */

    const double dt =
        clamp(
            deltaTimeSeconds,
            0.0001,
            0.1
        );

    result.rotation =
        motion.gyroZ * dt;

    result.translationX =
        motion.accelerationX *
        dt *
        dt *
        0.5;

    result.translationY =
        motion.accelerationY *
        dt *
        dt *
        0.5;

    const double motionMagnitude =
        std::sqrt(
            motion.gyroX * motion.gyroX +
            motion.gyroY * motion.gyroY +
            motion.gyroZ * motion.gyroZ
        );

    result.confidence =
        clamp(
            1.0 -
            motionMagnitude / 10.0,
            0.0,
            1.0
        );

    return result;
}

}
6. include/ExposureController.hpp
#pragma once

#include "CameraTypes.hpp"

namespace surface::camera {

class ExposureController
{
public:

    ExposureSettings calculate(
        const CameraState& state) const;

private:

    static double clamp(
        double value,
        double minimum,
        double maximum);
};

}
7. src/ExposureController.cpp
#include "ExposureController.hpp"

#include <algorithm>

namespace surface::camera {

double ExposureController::clamp(
    double value,
    double minimum,
    double maximum)
{
    return std::max(
        minimum,
        std::min(
            maximum,
            value
        )
    );
}

ExposureSettings
ExposureController::calculate(
    const CameraState& state) const
{
    ExposureSettings settings =
        state.exposure;

    if (
        settings.mode !=
        ExposureMode::Auto)
    {
        return settings;
    }

    const double lux =
        state.ambient.valid
            ? state.ambient.illuminanceLux
            : 200.0;

    /*
        Generic exposure model.

        Lower light:
            longer exposure + more gain.

        Higher light:
            shorter exposure + less gain.
    */

    if (lux < 5.0)
    {
        settings.exposureTimeMs =
            20.0;

        settings.gain =
            4.0;
    }
    else if (lux < 50.0)
    {
        settings.exposureTimeMs =
            12.0;

        settings.gain =
            2.0;
    }
    else if (lux < 500.0)
    {
        settings.exposureTimeMs =
            8.0;

        settings.gain =
            1.2;
    }
    else
    {
        settings.exposureTimeMs =
            3.0;

        settings.gain =
            1.0;
    }

    settings.exposureTimeMs =
        clamp(
            settings.exposureTimeMs,
            0.5,
            33.0
        );

    settings.gain =
        clamp(
            settings.gain,
            1.0,
            8.0
        );

    return settings;
}

}
8. include/ImagePipeline.hpp
#pragma once

#include "CameraFrame.hpp"

#include <functional>
#include <vector>

namespace surface::camera {

enum class ProcessingStage
{
    Denoise,
    Sharpen,
    WhiteBalance,
    ToneMapping,
    HDR,
    Stabilization,
    ColorCorrection
};

class ImagePipeline
{
public:

    CameraFramePtr process(
        const CameraFramePtr& input,
        const CameraPolicy& policy);

private:

    void processMetadata(
        CameraFrame& frame,
        const CameraPolicy& policy);
};

}
9. src/ImagePipeline.cpp
#include "ImagePipeline.hpp"

namespace surface::camera {

CameraFramePtr
ImagePipeline::process(
    const CameraFramePtr& input,
    const CameraPolicy& policy)
{
    if (!input)
    {
        return nullptr;
    }

    auto output =
        std::make_shared<CameraFrame>(
            *input
        );

    /*
        This layer intentionally doesn't perform
        fake pixel manipulation.

        Real processing would normally be backed by
        Media Foundation, Direct3D/DirectCompute,
        Media Foundation Transforms, or vendor camera
        processing components.
    */

    processMetadata(
        *output,
        policy
    );

    return output;
}

void ImagePipeline::processMetadata(
    CameraFrame& frame,
    const CameraPolicy& policy)
{
    frame.exposureTimeMs =
        policy.exposure.exposureTimeMs;

    frame.gain =
        policy.exposure.gain;

    frame.focusPosition =
        policy.focus.focusPosition;

    frame.hdr =
        policy.hdrEnabled;
}

}
10. include/CameraPolicy.hpp
#pragma once

#include "CameraTypes.hpp"

namespace surface::camera {

class CameraPolicyEngine
{
public:

    CameraPolicy calculate(
        const CameraState& state,
        const CameraCapabilities& capabilities) const;

private:

    static double clamp(
        double value,
        double minimum,
        double maximum);
};

}
11. src/CameraPolicy.cpp
#include "CameraPolicy.hpp"

#include "ExposureController.hpp"

#include <algorithm>

namespace surface::camera {

double CameraPolicyEngine::clamp(
    double value,
    double minimum,
    double maximum)
{
    return std::max(
        minimum,
        std::min(
            maximum,
            value
        )
    );
}

CameraPolicy
CameraPolicyEngine::calculate(
    const CameraState& state,
    const CameraCapabilities& capabilities) const
{
    CameraPolicy policy;

    /*
        Start from the current configuration.
    */

    policy.exposure =
        state.exposure;

    policy.focus =
        state.focus;

    policy.whiteBalance =
        state.whiteBalance;

    policy.targetResolution =
        state.resolution;

    policy.targetFrameRate =
        state.frameRate;

    policy.stabilizationEnabled =
        state.stabilizationEnabled;

    policy.hardwareProcessing =
        state.hardwareAcceleration;

    /*
        Privacy comes first.
    */

    if (
        state.privacy ==
        PrivacyState::CameraBlocked)
    {
        policy.targetFrameRate = 0.0;

        policy.reason =
            "Camera blocked by privacy state";

        return policy;
    }

    /*
        Low-light mode.
    */

    const double lux =
        state.ambient.valid
            ? state.ambient.illuminanceLux
            : 200.0;

    if (lux < 20.0)
    {
        policy.lowLightEnhancement =
            true;

        policy.noiseReduction =
            true;

        policy.stabilizationEnabled =
            true;

        policy.reason +=
            "Low-light processing; ";
    }

    /*
        HDR.
    */

    if (
        capabilities.hdr &&
        (
            state.mode ==
                CameraMode::Photo ||
            state.mode ==
                CameraMode::Video ||
            state.mode ==
                CameraMode::VideoConference
        ))
    {
        policy.hdrEnabled =
            true;

        policy.reason +=
            "HDR available; ";
    }

    /*
        Video conference optimisation.
    */

    if (
        state.mode ==
        CameraMode::VideoConference)
    {
        policy.targetFrameRate =
            30.0;

        policy.stabilizationEnabled =
            true;

        policy.noiseReduction =
            true;

        policy.reason +=
            "Video conference mode; ";
    }

    /*
        Video mode.
    */

    if (
        state.mode ==
        CameraMode::Video)
    {
        policy.targetFrameRate =
            30.0;

        policy.reason +=
            "Video mode; ";
    }

    /*
        Make sure requested frame rate exists
        in the camera's supported modes.
    */

    if (!capabilities.frameRates.empty())
    {
        double closest =
            capabilities.frameRates.front();

        double difference =
            std::abs(
                closest -
                policy.targetFrameRate
            );

        for (
            const double rate :
            capabilities.frameRates)
        {
            const double d =
                std::abs(
                    rate -
                    policy.targetFrameRate
                );

            if (d < difference)
            {
                difference = d;
                closest = rate;
            }
        }

        policy.targetFrameRate =
            closest;
    }

    /*
        Exposure.
    */

    ExposureController exposure;

    policy.exposure =
        exposure.calculate(state);

    return policy;
}

}
12. include/CameraEngine.hpp
#pragma once

#include "CameraDevice.hpp"
#include "CameraPolicy.hpp"
#include "ImagePipeline.hpp"
#include "SensorFusion.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace surface::camera {

class CameraEngine
{
public:

    using FrameCallback =
        std::function<void(
            const CameraFramePtr&)>;

    using PolicyCallback =
        std::function<void(
            const CameraPolicy&)>;

    CameraEngine(
        std::unique_ptr<ICameraDevice> device,
        CameraState initialState = {});

    ~CameraEngine();

    bool start();

    void stop();

    bool configure(
        const CameraState& state);

    CameraPolicy policy() const;

    CameraState state() const;

    void setFrameCallback(
        FrameCallback callback);

    void setPolicyCallback(
        PolicyCallback callback);

private:

    void captureLoop();

    std::unique_ptr<ICameraDevice>
        device_;

    CameraPolicyEngine
        policyEngine_;

    ImagePipeline
        imagePipeline_;

    SensorFusion
        sensorFusion_;

    mutable std::mutex mutex_;

    CameraState state_;

    CameraPolicy policy_;

    FrameCallback frameCallback_;

    PolicyCallback policyCallback_;

    std::atomic<bool>
        running_{false};

    std::thread worker_;
};

}
13. src/CameraEngine.cpp
#include "CameraEngine.hpp"

#include <chrono>

namespace surface::camera {

CameraEngine::CameraEngine(
    std::unique_ptr<ICameraDevice> device,
    CameraState initialState)
    : device_(std::move(device)),
      state_(initialState)
{
}

CameraEngine::~CameraEngine()
{
    stop();
}

bool CameraEngine::start()
{
    if (!device_)
    {
        return false;
    }

    if (!device_->open())
    {
        return false;
    }

    {
        std::lock_guard lock(mutex_);

        policy_ =
            policyEngine_.calculate(
                state_,
                device_->capabilities()
            );
    }

    if (!device_->configure(state_))
    {
        device_->close();
        return false;
    }

    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return true;
    }

    worker_ =
        std::thread(
            &CameraEngine::captureLoop,
            this
        );

    return true;
}

void CameraEngine::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }

    if (device_)
    {
        device_->close();
    }
}

bool CameraEngine::configure(
    const CameraState& state)
{
    if (!device_)
    {
        return false;
    }

    if (!device_->configure(state))
    {
        return false;
    }

    CameraPolicy newPolicy =
        policyEngine_.calculate(
            state,
            device_->capabilities()
        );

    {
        std::lock_guard lock(mutex_);

        state_ = state;

        policy_ =
            newPolicy;
    }

    if (policyCallback_)
    {
        policyCallback_(newPolicy);
    }

    return true;
}

void CameraEngine::captureLoop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        CameraFramePtr frame =
            device_->captureFrame();

        if (frame)
        {
            CameraPolicy currentPolicy;

            FrameCallback callback;

            {
                std::lock_guard lock(mutex_);

                currentPolicy =
                    policy_;

                callback =
                    frameCallback_;
            }

            auto processed =
                imagePipeline_.process(
                    frame,
                    currentPolicy
                );

            if (
                processed &&
                callback)
            {
                callback(processed);
            }
        }

        std::this_thread::sleep_for(
            1ms
        );
    }
}

bool CameraEngine::configure(
    const CameraState& state);

CameraPolicy
CameraEngine::policy() const
{
    std::lock_guard lock(mutex_);

    return policy_;
}

CameraState
CameraEngine::state() const
{
    std::lock_guard lock(mutex_);

    return state_;
}

void CameraEngine::setFrameCallback(
    FrameCallback callback)
{
    std::lock_guard lock(mutex_);

    frameCallback_ =
        std::move(callback);
}

void CameraEngine::setPolicyCallback(
    PolicyCallback callback)
{
    std::lock_guard lock(mutex_);

    policyCallback_ =
        std::move(callback);
}

}

There is one redundant declaration in the above implementation that should be removed:

bool CameraEngine::configure(
    const CameraState& state);

The cleaned-up CameraEngine.cpp should simply omit that line.

14. include/WindowsCameraBackend.hpp
#pragma once

#include "CameraDevice.hpp"

namespace surface::camera {

class WindowsCameraBackend final
    : public ICameraDevice
{
public:

    bool open() override;

    void close() override;

    bool isOpen() const override;

    CameraCapabilities
    capabilities() const override;

    bool configure(
        const CameraState& state) override;

    CameraFramePtr
    captureFrame() override;

private:

    bool open_{false};

    CameraState state_;

    CameraCapabilities capabilities_;

    std::uint64_t frameNumber_{0};
};

}
15. src/WindowsCameraBackend.cpp
#include "WindowsCameraBackend.hpp"

#include <algorithm>

namespace surface::camera {

bool WindowsCameraBackend::open()
{
    /*
        Production implementation:
        connect this class to the supported Windows
        camera capture stack.

        The hardware driver remains responsible for
        actual sensor access.
    */

    open_ = true;

    capabilities_.resolutions =
    {
        {640, 480},
        {1280, 720},
        {1920, 1080}
    };

    capabilities_.frameRates =
    {
        24.0,
        30.0,
        60.0
    };

    capabilities_.maxResolution =
        {1920, 1080};

    capabilities_.autofocus =
        true;

    capabilities_.autoExposure =
        true;

    capabilities_.autoWhiteBalance =
        true;

    capabilities_.hdr =
        true;

    capabilities_.hardwareVideoProcessing =
        true;

    return true;
}

void WindowsCameraBackend::close()
{
    open_ = false;
}

bool WindowsCameraBackend::isOpen() const
{
    return open_;
}

CameraCapabilities
WindowsCameraBackend::capabilities() const
{
    return capabilities_;
}

bool WindowsCameraBackend::configure(
    const CameraState& state)
{
    if (!open_)
    {
        return false;
    }

    /*
        Validate resolution.
    */

    const bool resolutionSupported =
        std::find(
            capabilities_.resolutions.begin(),
            capabilities_.resolutions.end(),
            state.resolution
        ) !=
        capabilities_.resolutions.end();

    if (!resolutionSupported)
    {
        return false;
    }

    /*
        Validate frame rate.
    */

    const bool frameRateSupported =
        std::find(
            capabilities_.frameRates.begin(),
            capabilities_.frameRates.end(),
            state.frameRate
        ) !=
        capabilities_.frameRates.end();

    if (!frameRateSupported)
    {
        return false;
    }

    state_ = state;

    return true;
}

CameraFramePtr
WindowsCameraBackend::captureFrame()
{
    if (!open_)
    {
        return nullptr;
    }

    /*
        The real implementation should obtain the
        sample from the Windows camera pipeline.

        We deliberately don't manufacture image bytes.
    */

    return nullptr;
}

}
16. src/main.cpp
#include "CameraEngine.hpp"
#include "WindowsCameraBackend.hpp"

#include <iostream>
#include <memory>

using namespace surface::camera;

int main()
{
    CameraState configuration;

    configuration.type =
        CameraType::Front;

    configuration.mode =
        CameraMode::VideoConference;

    configuration.resolution =
        {1920, 1080};

    configuration.frameRate =
        30.0;

    configuration.format =
        PixelFormat::NV12;

    configuration.focus.mode =
        FocusMode::Continuous;

    configuration.exposure.mode =
        ExposureMode::Auto;

    configuration.whiteBalance.mode =
        WhiteBalanceMode::Auto;

    configuration.stabilizationEnabled =
        true;

    configuration.hardwareAcceleration =
        true;

    auto backend =
        std::make_unique<
            WindowsCameraBackend
        >();

    CameraEngine engine(
        std::move(backend),
        configuration
    );

    engine.setPolicyCallback(
        [](const CameraPolicy& policy)
        {
            std::cout
                << "\n================================\n"
                << " CAMERA POLICY\n"
                << "================================\n"

                << "Frame rate: "
                << policy.targetFrameRate
                << " FPS\n"

                << "Exposure: "
                << policy.exposure.exposureTimeMs
                << " ms\n"

                << "Gain: "
                << policy.exposure.gain
                << "\n"

                << "HDR: "
                << (
                    policy.hdrEnabled
                    ? "YES"
                    : "NO"
                )

                << "\nStabilization: "
                << (
                    policy.stabilizationEnabled
                    ? "YES"
                    : "NO"
                )

                << "\nLow-light: "
                << (
                    policy.lowLightEnhancement
                    ? "YES"
                    : "NO"
                )

                << "\nReason: "
                << policy.reason
                << "\n";
        }
    );

    engine.setFrameCallback(
        [](const CameraFramePtr& frame)
        {
            if (!frame)
            {
                return;
            }

            std::cout
                << "Frame #"
                << frame->frameNumber
                << " received\n";
        }
    );

    if (!engine.start())
    {
        std::cerr
            << "Failed to start camera.\n";

        return 1;
    }

    std::cout
        << "Surface Camera Engine running.\n";

    std::cout
        << "Press ENTER to stop.\n";

    std::cin.get();

    engine.stop();

    return 0;
}
17. tests/CameraTests.cpp
#include "CameraPolicy.hpp"
#include "ExposureController.hpp"
#include "SensorFusion.hpp"

#include <cassert>
#include <iostream>

using namespace surface::camera;

int main()
{
    /*
        Low-light exposure.
    */

    CameraState lowLight;

    lowLight.ambient.valid =
        true;

    lowLight.ambient.illuminanceLux =
        2.0;

    ExposureController exposure;

    const auto exposureSettings =
        exposure.calculate(
            lowLight
        );

    assert(
        exposureSettings.exposureTimeMs >
        10.0
    );

    assert(
        exposureSettings.gain >
        1.0
    );

    /*
        Normal light.
    */

    CameraState normal;

    normal.ambient.valid =
        true;

    normal.ambient.illuminanceLux =
        500.0;

    const auto normalExposure =
        exposure.calculate(
            normal
        );

    assert(
        normalExposure.gain <=
        1.5
    );

    /*
        Sensor fusion.
    */

    MotionSensorData motion;

    motion.valid =
        true;

    motion.gyroZ =
        1.0;

    SensorFusion fusion;

    auto stabilization =
        fusion.estimate(
            motion,
            0.016
        );

    assert(
        stabilization.rotation >
        0.0
    );

    /*
        Camera policy.
    */

    CameraCapabilities capabilities;

    capabilities.hdr =
        true;

    capabilities.resolutions =
    {
        {1280, 720},
        {1920, 1080}
    };

    capabilities.frameRates =
    {
        30.0,
        60.0
    };

    CameraState state;

    state.mode =
        CameraMode::VideoConference;

    state.resolution =
        {1920, 1080};

    state.frameRate =
        30.0;

    state.ambient.valid =
        true;

    state.ambient.illuminanceLux =
        10.0;

    CameraPolicyEngine policyEngine;

    const auto policy =
        policyEngine.calculate(
            state,
            capabilities
        );

    assert(
        policy.targetFrameRate ==
        30.0
    );

    assert(
        policy.lowLightEnhancement
    );

    assert(
        policy.stabilizationEnabled
    );

    /*
        Privacy protection.
    */

    state.privacy =
        PrivacyState::CameraBlocked;

    const auto blockedPolicy =
        policyEngine.calculate(
            state,
            capabilities
        );

    assert(
        blockedPolicy.targetFrameRate ==
        0.0
    );

    std::cout
        << "All camera tests passed.\n";

    return 0;
}
18. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceCameraEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceCameraEngine

    src/main.cpp
    src/CameraDevice.cpp
    src/SensorFusion.cpp
    src/ExposureController.cpp
    src/ImagePipeline.cpp
    src/CameraPolicy.cpp
    src/CameraEngine.cpp
    src/WindowsCameraBackend.cpp
)

target_include_directories(
    SurfaceCameraEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceCameraEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    CameraTests

    tests/CameraTests.cpp
    src/SensorFusion.cpp
    src/ExposureController.cpp
    src/CameraPolicy.cpp
)

target_include_directories(
    CameraTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME CameraTests
    COMMAND CameraTests
)




Project structure
SurfacePerformanceEngine/
├── CMakeLists.txt
├── include/
│   ├── PerformanceTypes.hpp
│   ├── PerformanceTelemetry.hpp
│   ├── PerformanceModel.hpp
│   ├── PerformancePolicy.hpp
│   ├── WorkloadManager.hpp
│   ├── ResourceGovernor.hpp
│   ├── PerformanceEngine.hpp
│   └── WindowsPerformanceBackend.hpp
├── src/
│   ├── PerformanceModel.cpp
│   ├── PerformancePolicy.cpp
│   ├── WorkloadManager.cpp
│   ├── ResourceGovernor.cpp
│   ├── PerformanceEngine.cpp
│   ├── WindowsPerformanceBackend.cpp
│   └── main.cpp
└── tests/
    └── PerformanceTests.cpp
1. include/PerformanceTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace surface::performance {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class PerformanceMode
{
    BatterySaver,
    Balanced,
    Responsive,
    Performance,
    Gaming,
    Creative,
    Presentation
};

enum class ResourceType
{
    CPU,
    GPU,
    NPU,
    Memory,
    Storage,
    Network
};

enum class WorkloadType
{
    Idle,
    General,
    Browser,
    Office,
    VideoPlayback,
    VideoConference,
    Gaming,
    Rendering,
    Compilation,
    MachineLearning,
    Background,
    FileTransfer
};

enum class Priority
{
    Background,
    Normal,
    High,
    Realtime
};

enum class GovernorAction
{
    Maintain,
    Boost,
    Reduce,
    Migrate,
    Suspend,
    Resume
};

struct CPUState
{
    double utilizationPercent{0.0};

    double frequencyGHz{0.0};

    double temperatureC{0.0};

    std::uint32_t activeThreads{0};

    std::uint32_t logicalProcessors{0};
};

struct GPUState
{
    double utilizationPercent{0.0};

    double frequencyGHz{0.0};

    double temperatureC{0.0};

    std::uint64_t memoryUsedMB{0};

    std::uint64_t memoryTotalMB{0};
};

struct NPUState
{
    double utilizationPercent{0.0};

    double temperatureC{0.0};

    bool available{false};
};

struct MemoryState
{
    std::uint64_t totalMB{0};

    std::uint64_t usedMB{0};

    double utilizationPercent{0.0};
};

struct StorageState
{
    double utilizationPercent{0.0};

    double readMBps{0.0};

    double writeMBps{0.0};
};

struct NetworkState
{
    double utilizationPercent{0.0};

    double downloadMbps{0.0};

    double uploadMbps{0.0};

    bool connected{false};
};

struct SystemTelemetry
{
    CPUState cpu;

    GPUState gpu;

    NPUState npu;

    MemoryState memory;

    StorageState storage;

    NetworkState network;

    double batteryPercent{100.0};

    bool onACPower{true};

    double systemTemperatureC{0.0};

    TimePoint timestamp{
        Clock::now()
    };
};

struct Workload
{
    std::uint64_t id{0};

    std::string name;

    WorkloadType type{
        WorkloadType::General
    };

    Priority priority{
        Priority::Normal
    };

    double cpuDemand{0.0};

    double gpuDemand{0.0};

    double npuDemand{0.0};

    double memoryDemandMB{0.0};

    double networkDemandMbps{0.0};

    bool latencySensitive{false};

    bool background{false};

    bool active{true};
};

struct ResourceLimits
{
    double maxCPUPercent{100.0};

    double maxGPUPercent{100.0};

    double maxNPUPercent{100.0};

    double maxMemoryPercent{100.0};

    double maxNetworkMbps{1000.0};
};

struct PerformancePolicy
{
    PerformanceMode mode{
        PerformanceMode::Balanced
    };

    ResourceLimits limits;

    double cpuBoostFactor{1.0};

    double gpuBoostFactor{1.0};

    double npuBoostFactor{1.0};

    double memoryReservePercent{10.0};

    bool allowBackgroundWork{true};

    bool preferEfficiency{false};

    bool preferLatency{false};

    std::string reason;
};

struct PerformanceSnapshot
{
    SystemTelemetry telemetry;

    PerformancePolicy policy;

    double performanceScore{0.0};

    double efficiencyScore{0.0};
};

}
2. include/PerformanceTelemetry.hpp
#pragma once

#include "PerformanceTypes.hpp"

namespace surface::performance {

class IPerformanceTelemetry
{
public:

    virtual ~IPerformanceTelemetry() = default;

    virtual SystemTelemetry read() = 0;
};

}
3. include/PerformanceModel.hpp
#pragma once

#include "PerformanceTypes.hpp"

namespace surface::performance {

class PerformanceModel
{
public:

    double performanceScore(
        const SystemTelemetry& telemetry) const;

    double efficiencyScore(
        const SystemTelemetry& telemetry) const;

    double thermalHeadroom(
        const SystemTelemetry& telemetry) const;

    bool systemUnderPressure(
        const SystemTelemetry& telemetry) const;
};

}
4. src/PerformanceModel.cpp
#include "PerformanceModel.hpp"

#include <algorithm>

namespace surface::performance {

double PerformanceModel::performanceScore(
    const SystemTelemetry& t) const
{
    const double cpu =
        t.cpu.utilizationPercent;

    const double gpu =
        t.gpu.utilizationPercent;

    const double npu =
        t.npu.utilizationPercent;

    /*
        A simple utilisation-derived system score.

        This isn't intended to claim that utilisation
        itself equals performance. It provides a useful
        control signal for the governor.
    */

    const double score =
        cpu * 0.35 +
        gpu * 0.35 +
        npu * 0.30;

    return std::clamp(
        score,
        0.0,
        100.0
    );
}

double PerformanceModel::efficiencyScore(
    const SystemTelemetry& t) const
{
    const double thermalPenalty =
        std::max(
            0.0,
            t.systemTemperatureC - 50.0
        );

    const double batteryPenalty =
        t.onACPower
            ? 0.0
            : (100.0 - t.batteryPercent) * 0.3;

    const double score =
        100.0 -
        thermalPenalty -
        batteryPenalty;

    return std::clamp(
        score,
        0.0,
        100.0
    );
}

double PerformanceModel::thermalHeadroom(
    const SystemTelemetry& t) const
{
    constexpr double thermalLimit = 100.0;

    return std::clamp(
        thermalLimit -
        t.systemTemperatureC,
        0.0,
        thermalLimit
    );
}

bool PerformanceModel::systemUnderPressure(
    const SystemTelemetry& t) const
{
    if (t.systemTemperatureC >= 90.0)
    {
        return true;
    }

    if (t.memory.utilizationPercent >= 95.0)
    {
        return true;
    }

    if (t.cpu.utilizationPercent >= 98.0 &&
        t.gpu.utilizationPercent >= 98.0)
    {
        return true;
    }

    return false;
}

}
5. include/PerformancePolicy.hpp
#pragma once

#include "PerformanceTypes.hpp"

namespace surface::performance {

class PerformancePolicyEngine
{
public:

    PerformancePolicy calculate(
        const SystemTelemetry& telemetry,
        const std::vector<Workload>& workloads) const;

private:

    static bool containsWorkload(
        const std::vector<Workload>& workloads,
        WorkloadType type);
};

}
6. src/PerformancePolicy.cpp
#include "PerformancePolicy.hpp"

#include <algorithm>

namespace surface::performance {

bool PerformancePolicyEngine::containsWorkload(
    const std::vector<Workload>& workloads,
    WorkloadType type)
{
    for (const auto& workload : workloads)
    {
        if (
            workload.active &&
            workload.type == type)
        {
            return true;
        }
    }

    return false;
}

PerformancePolicy
PerformancePolicyEngine::calculate(
    const SystemTelemetry& telemetry,
    const std::vector<Workload>& workloads) const
{
    PerformancePolicy policy;

    /*
        Default balanced operation.
    */

    policy.mode =
        PerformanceMode::Balanced;

    policy.cpuBoostFactor =
        1.0;

    policy.gpuBoostFactor =
        1.0;

    policy.npuBoostFactor =
        1.0;

    policy.allowBackgroundWork =
        true;

    policy.preferEfficiency =
        false;

    /*
        Battery saver.
    */

    if (
        !telemetry.onACPower &&
        telemetry.batteryPercent <= 20.0)
    {
        policy.mode =
            PerformanceMode::BatterySaver;

        policy.cpuBoostFactor =
            0.75;

        policy.gpuBoostFactor =
            0.70;

        policy.npuBoostFactor =
            0.85;

        policy.allowBackgroundWork =
            false;

        policy.preferEfficiency =
            true;

        policy.reason +=
            "Low battery; ";
    }

    /*
        Gaming.
    */

    if (
        containsWorkload(
            workloads,
            WorkloadType::Gaming))
    {
        policy.mode =
            PerformanceMode::Gaming;

        policy.cpuBoostFactor =
            1.15;

        policy.gpuBoostFactor =
            1.20;

        policy.npuBoostFactor =
            1.05;

        policy.preferLatency =
            true;

        policy.reason +=
            "Gaming workload; ";
    }

    /*
        Creative workloads.
    */

    if (
        containsWorkload(
            workloads,
            WorkloadType::Rendering) ||
        containsWorkload(
            workloads,
            WorkloadType::Compilation))
    {
        policy.mode =
            PerformanceMode::Creative;

        policy.cpuBoostFactor =
            1.15;

        policy.gpuBoostFactor =
            1.15;

        policy.npuBoostFactor =
            1.10;

        policy.reason +=
            "Creative workload; ";
    }

    /*
        Machine learning.
    */

    if (
        containsWorkload(
            workloads,
            WorkloadType::MachineLearning))
    {
        policy.npuBoostFactor =
            1.20;

        policy.gpuBoostFactor =
            1.10;

        policy.reason +=
            "AI workload; ";
    }

    /*
        Video conference.
    */

    if (
        containsWorkload(
            workloads,
            WorkloadType::VideoConference))
    {
        policy.mode =
            PerformanceMode::Responsive;

        policy.cpuBoostFactor =
            1.05;

        policy.gpuBoostFactor =
            1.05;

        policy.npuBoostFactor =
            1.10;

        policy.preferLatency =
            true;

        policy.reason +=
            "Video conference; ";
    }

    /*
        Thermal protection overrides performance modes.
    */

    if (
        telemetry.systemTemperatureC >=
        90.0)
    {
        policy.mode =
            PerformanceMode::BatterySaver;

        policy.cpuBoostFactor =
            0.70;

        policy.gpuBoostFactor =
            0.65;

        policy.npuBoostFactor =
            0.75;

        policy.allowBackgroundWork =
            false;

        policy.preferEfficiency =
            true;

        policy.reason +=
            "Thermal protection; ";
    }

    /*
        Memory pressure.
    */

    if (
        telemetry.memory.utilizationPercent >=
        95.0)
    {
        policy.memoryReservePercent =
            15.0;

        policy.allowBackgroundWork =
            false;

        policy.reason +=
            "Memory pressure; ";
    }

    /*
        Resource limits.
    */

    policy.limits.maxCPUPercent =
        100.0 *
        policy.cpuBoostFactor;

    policy.limits.maxGPUPercent =
        100.0 *
        policy.gpuBoostFactor;

    policy.limits.maxNPUPercent =
        100.0 *
        policy.npuBoostFactor;

    return policy;
}

}
7. include/WorkloadManager.hpp
#pragma once

#include "PerformanceTypes.hpp"

#include <mutex>
#include <vector>

namespace surface::performance {

class WorkloadManager
{
public:

    std::uint64_t registerWorkload(
        Workload workload);

    bool removeWorkload(
        std::uint64_t id);

    bool setActive(
        std::uint64_t id,
        bool active);

    std::vector<Workload>
    workloads() const;

private:

    mutable std::mutex mutex_;

    std::vector<Workload>
        workloads_;

    std::uint64_t nextId_{1};
};

}
8. src/WorkloadManager.cpp
#include "WorkloadManager.hpp"

#include <algorithm>

namespace surface::performance {

std::uint64_t
WorkloadManager::registerWorkload(
    Workload workload)
{
    std::lock_guard lock(mutex_);

    workload.id =
        nextId_++;

    workloads_.push_back(
        std::move(workload)
    );

    return workloads_.back().id;
}

bool WorkloadManager::removeWorkload(
    std::uint64_t id)
{
    std::lock_guard lock(mutex_);

    const auto oldSize =
        workloads_.size();

    workloads_.erase(
        std::remove_if(
            workloads_.begin(),
            workloads_.end(),
            [id](const Workload& workload)
            {
                return workload.id == id;
            }
        ),
        workloads_.end()
    );

    return workloads_.size() != oldSize;
}

bool WorkloadManager::setActive(
    std::uint64_t id,
    bool active)
{
    std::lock_guard lock(mutex_);

    for (auto& workload : workloads_)
    {
        if (workload.id == id)
        {
            workload.active =
                active;

            return true;
        }
    }

    return false;
}

std::vector<Workload>
WorkloadManager::workloads() const
{
    std::lock_guard lock(mutex_);

    return workloads_;
}

}
9. include/ResourceGovernor.hpp
#pragma once

#include "PerformanceTypes.hpp"

namespace surface::performance {

struct ResourceDecision
{
    ResourceType resource;

    GovernorAction action{
        GovernorAction::Maintain
    };

    double factor{1.0};

    std::string reason;
};

class ResourceGovernor
{
public:

    ResourceDecision evaluateCPU(
        const SystemTelemetry& telemetry,
        const PerformancePolicy& policy) const;

    ResourceDecision evaluateGPU(
        const SystemTelemetry& telemetry,
        const PerformancePolicy& policy) const;

    ResourceDecision evaluateNPU(
        const SystemTelemetry& telemetry,
        const PerformancePolicy& policy) const;

    ResourceDecision evaluateMemory(
        const SystemTelemetry& telemetry,
        const PerformancePolicy& policy) const;
};

}
10. src/ResourceGovernor.cpp
#include "ResourceGovernor.hpp"

namespace surface::performance {

ResourceDecision
ResourceGovernor::evaluateCPU(
    const SystemTelemetry& telemetry,
    const PerformancePolicy& policy) const
{
    ResourceDecision decision;

    decision.resource =
        ResourceType::CPU;

    decision.factor =
        policy.cpuBoostFactor;

    if (
        telemetry.cpu.utilizationPercent >=
        98.0)
    {
        decision.action =
            GovernorAction::Reduce;

        decision.factor =
            0.75;

        decision.reason =
            "CPU saturation";
    }
    else if (
        policy.preferLatency &&
        telemetry.cpu.utilizationPercent < 80.0)
    {
        decision.action =
            GovernorAction::Boost;

        decision.reason =
            "Latency-sensitive workload";
    }

    return decision;
}

ResourceDecision
ResourceGovernor::evaluateGPU(
    const SystemTelemetry& telemetry,
    const PerformancePolicy& policy) const
{
    ResourceDecision decision;

    decision.resource =
        ResourceType::GPU;

    decision.factor =
        policy.gpuBoostFactor;

    if (
        telemetry.gpu.utilizationPercent >=
        98.0)
    {
        decision.action =
            GovernorAction::Reduce;

        decision.factor =
            0.85;

        decision.reason =
            "GPU saturation";
    }
    else if (
        policy.mode ==
        PerformanceMode::Gaming)
    {
        decision.action =
            GovernorAction::Boost;

        decision.reason =
            "Gaming workload";
    }

    return decision;
}

ResourceDecision
ResourceGovernor::evaluateNPU(
    const SystemTelemetry& telemetry,
    const PerformancePolicy& policy) const
{
    ResourceDecision decision;

    decision.resource =
        ResourceType::NPU;

    decision.factor =
        policy.npuBoostFactor;

    if (!telemetry.npu.available)
    {
        decision.action =
            GovernorAction::Migrate;

        decision.factor =
            1.0;

        decision.reason =
            "NPU unavailable";
    }
    else if (
        telemetry.npu.utilizationPercent >=
        98.0)
    {
        decision.action =
            GovernorAction::Reduce;

        decision.factor =
            0.80;

        decision.reason =
            "NPU saturation";
    }

    return decision;
}

ResourceDecision
ResourceGovernor::evaluateMemory(
    const SystemTelemetry& telemetry,
    const PerformancePolicy& policy) const
{
    ResourceDecision decision;

    decision.resource =
        ResourceType::Memory;

    decision.factor =
        1.0;

    if (
        telemetry.memory.utilizationPercent >=
        95.0)
    {
        decision.action =
            GovernorAction::Reduce;

        decision.reason =
            "Memory pressure";
    }

    return decision;
}

}
11. include/PerformanceEngine.hpp
#pragma once

#include "PerformanceModel.hpp"
#include "PerformancePolicy.hpp"
#include "PerformanceTelemetry.hpp"
#include "ResourceGovernor.hpp"
#include "WorkloadManager.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace surface::performance {

class PerformanceEngine
{
public:

    using SnapshotCallback =
        std::function<void(
            const PerformanceSnapshot&)>;

    explicit PerformanceEngine(
        std::unique_ptr<
            IPerformanceTelemetry
        > telemetry);

    ~PerformanceEngine();

    void start();

    void stop();

    void update();

    void registerWorkload(
        Workload workload);

    PerformanceSnapshot
    snapshot() const;

    void setCallback(
        SnapshotCallback callback);

private:

    void loop();

    std::unique_ptr<
        IPerformanceTelemetry
    > telemetry_;

    WorkloadManager
        workloadManager_;

    PerformanceModel
        model_;

    PerformancePolicyEngine
        policyEngine_;

    ResourceGovernor
        governor_;

    mutable std::mutex mutex_;

    PerformanceSnapshot
        latestSnapshot_;

    SnapshotCallback
        callback_;

    std::atomic<bool>
        running_{false};

    std::thread worker_;
};

}
12. src/PerformanceEngine.cpp
#include "PerformanceEngine.hpp"

#include <chrono>

namespace surface::performance {

PerformanceEngine::PerformanceEngine(
    std::unique_ptr<
        IPerformanceTelemetry
    > telemetry)
    : telemetry_(std::move(telemetry))
{
}

PerformanceEngine::~PerformanceEngine()
{
    stop();
}

void PerformanceEngine::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    worker_ =
        std::thread(
            &PerformanceEngine::loop,
            this
        );
}

void PerformanceEngine::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }
}

void PerformanceEngine::registerWorkload(
    Workload workload)
{
    workloadManager_
        .registerWorkload(
            std::move(workload)
        );
}

void PerformanceEngine::update()
{
    if (!telemetry_)
    {
        return;
    }

    SystemTelemetry telemetry =
        telemetry_->read();

    const auto workloads =
        workloadManager_.workloads();

    PerformancePolicy policy =
        policyEngine_.calculate(
            telemetry,
            workloads
        );

    PerformanceSnapshot snapshot;

    snapshot.telemetry =
        telemetry;

    snapshot.policy =
        policy;

    snapshot.performanceScore =
        model_.performanceScore(
            telemetry
        );

    snapshot.efficiencyScore =
        model_.efficiencyScore(
            telemetry
        );

    /*
        Evaluate resources.

        These decisions can subsequently be connected
        to supported Windows power/performance controls.
    */

    const auto cpuDecision =
        governor_.evaluateCPU(
            telemetry,
            policy
        );

    const auto gpuDecision =
        governor_.evaluateGPU(
            telemetry,
            policy
        );

    const auto npuDecision =
        governor_.evaluateNPU(
            telemetry,
            policy
        );

    const auto memoryDecision =
        governor_.evaluateMemory(
            telemetry,
            policy
        );

    /*
        Keep the decisions alive in this layer for
        debugging/extension. A production implementation
        would publish them to the appropriate Windows
        resource-control backend.
    */

    (void)cpuDecision;
    (void)gpuDecision;
    (void)npuDecision;
    (void)memoryDecision;

    SnapshotCallback callback;

    {
        std::lock_guard lock(mutex_);

        latestSnapshot_ =
            snapshot;

        callback =
            callback_;
    }

    if (callback)
    {
        callback(snapshot);
    }
}

void PerformanceEngine::loop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        update();

        /*
            100 ms gives the system governor a reasonably
            responsive control loop without pretending to
            be a kernel scheduler.
        */

        std::this_thread::sleep_for(
            100ms
        );
    }
}

PerformanceSnapshot
PerformanceEngine::snapshot() const
{
    std::lock_guard lock(mutex_);

    return latestSnapshot_;
}

void PerformanceEngine::setCallback(
    SnapshotCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}

}
13. include/WindowsPerformanceBackend.hpp
#pragma once

#include "PerformanceTelemetry.hpp"

namespace surface::performance {

class WindowsPerformanceBackend final
    : public IPerformanceTelemetry
{
public:

    SystemTelemetry read() override;
};

}
14. src/WindowsPerformanceBackend.cpp
#include "WindowsPerformanceBackend.hpp"

#ifdef _WIN32

#include <windows.h>

#endif

namespace surface::performance {

SystemTelemetry
WindowsPerformanceBackend::read()
{
    SystemTelemetry telemetry;

#ifdef _WIN32

    /*
        Basic power state available through Win32.
    */

    SYSTEM_POWER_STATUS power{};

    if (GetSystemPowerStatus(&power))
    {
        telemetry.batteryPercent =
            static_cast<double>(
                power.BatteryLifePercent
            );

        telemetry.onACPower =
            power.ACLineStatus == 1;
    }

#endif

    /*
        CPU/GPU/NPU/memory/storage/network metrics
        should be populated through appropriate
        supported Windows telemetry interfaces.

        We deliberately don't fabricate values.

        Examples of production integrations include:
        - Windows performance counters
        - ETW
        - DXGI
        - Direct3D telemetry
        - Windows power-management APIs
        - Memory Manager information
        - IP Helper / networking APIs
        - OEM/platform telemetry where documented
    */

    return telemetry;
}

}
15. src/main.cpp
#include "PerformanceEngine.hpp"
#include "WindowsPerformanceBackend.hpp"

#include <iostream>
#include <memory>

using namespace surface::performance;

int main()
{
    auto telemetry =
        std::make_unique<
            WindowsPerformanceBackend
        >();

    PerformanceEngine engine(
        std::move(telemetry)
    );

    /*
        Browser workload.
    */

    engine.registerWorkload(
        {
            0,
            "Browser",
            WorkloadType::Browser,
            Priority::High,

            20.0,
            10.0,
            0.0,

            1200.0,
            5.0,

            true,
            false,
            true
        }
    );

    /*
        AI workload.
    */

    engine.registerWorkload(
        {
            0,
            "AI Inference",
            WorkloadType::MachineLearning,
            Priority::High,

            20.0,
            40.0,
            80.0,

            3000.0,
            10.0,

            true,
            false,
            true
        }
    );

    /*
        Background backup.
    */

    engine.registerWorkload(
        {
            0,
            "Backup",
            WorkloadType::Background,
            Priority::Background,

            10.0,
            0.0,
            0.0,

            500.0,
            100.0,

            false,
            true,
            true
        }
    );

    engine.setCallback(
        [](const PerformanceSnapshot& snapshot)
        {
            std::cout
                << "\n====================================\n"
                << " SURFACE PERFORMANCE ENGINE\n"
                << "====================================\n"

                << "CPU: "
                << snapshot.telemetry.cpu.utilizationPercent
                << "%\n"

                << "GPU: "
                << snapshot.telemetry.gpu.utilizationPercent
                << "%\n"

                << "NPU: "
                << snapshot.telemetry.npu.utilizationPercent
                << "%\n"

                << "Memory: "
                << snapshot.telemetry.memory.utilizationPercent
                << "%\n"

                << "Battery: "
                << snapshot.telemetry.batteryPercent
                << "%\n"

                << "Performance score: "
                << snapshot.performanceScore
                << "\n"

                << "Efficiency score: "
                << snapshot.efficiencyScore
                << "\n"

                << "Reason: "
                << snapshot.policy.reason
                << "\n";
        }
    );

    engine.start();

    std::cout
        << "Surface Performance Engine running.\n";

    std::cout
        << "Press ENTER to stop.\n";

    std::cin.get();

    engine.stop();

    return 0;
}
16. tests/PerformanceTests.cpp
#include "PerformanceModel.hpp"
#include "PerformancePolicy.hpp"
#include "ResourceGovernor.hpp"

#include <cassert>
#include <iostream>

using namespace surface::performance;

int main()
{
    PerformanceModel model;

    /*
        Thermal headroom.
    */

    SystemTelemetry normal;

    normal.systemTemperatureC =
        50.0;

    assert(
        model.thermalHeadroom(normal)
        == 50.0
    );

    /*
        Thermal pressure.
    */

    SystemTelemetry hot;

    hot.systemTemperatureC =
        95.0;

    assert(
        model.systemUnderPressure(hot)
    );

    /*
        Battery saver policy.
    */

    ConnectivityPolicyEngine_UNUSED:

    PerformancePolicyEngine
        policyEngine;

    SystemTelemetry battery;

    battery.onACPower =
        false;

    battery.batteryPercent =
        10.0;

    std::vector<Workload> workloads;

    const auto batteryPolicy =
        policyEngine.calculate(
            battery,
            workloads
        );

    assert(
        batteryPolicy.mode ==
        PerformanceMode::BatterySaver
    );

    assert(
        batteryPolicy.preferEfficiency
    );

    /*
        Gaming policy.
    */

    workloads.push_back(
        {
            1,
            "Game",
            WorkloadType::Gaming,
            Priority::High,

            60.0,
            80.0,
            0.0,

            4000.0,
            5.0,

            true,
            false,
            true
        }
    );

    SystemTelemetry gaming;

    gaming.onACPower =
        true;

    gaming.batteryPercent =
        100.0;

    const auto gamingPolicy =
        policyEngine.calculate(
            gaming,
            workloads
        );

    assert(
        gamingPolicy.mode ==
        PerformanceMode::Gaming
    );

    assert(
        gamingPolicy.gpuBoostFactor >
        1.0
    );

    /*
        Thermal protection should override
        the performance-oriented policy.
    */

    gaming.systemTemperatureC =
        95.0;

    const auto thermalPolicy =
        policyEngine.calculate(
            gaming,
            workloads
        );

    assert(
        thermalPolicy.mode ==
        PerformanceMode::BatterySaver
    );

    assert(
        thermalPolicy.gpuBoostFactor <
        1.0
    );

    /*
        NPU fallback.
    */

    ResourceGovernor governor;

    SystemTelemetry npuUnavailable;

    npuUnavailable.npu.available =
        false;

    PerformancePolicy balanced;

    const auto npuDecision =
        governor.evaluateNPU(
            npuUnavailable,
            balanced
        );

    assert(
        npuDecision.action ==
        GovernorAction::Migrate
    );

    std::cout
        << "All performance tests passed.\n";

    return 0;
}

There is one accidental unused label in that test:

ConnectivityPolicyEngine_UNUSED:

Remove it. The clean section is simply:

PerformancePolicyEngine
    policyEngine;
17. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfacePerformanceEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfacePerformanceEngine

    src/main.cpp
    src/PerformanceModel.cpp
    src/PerformancePolicy.cpp
    src/WorkloadManager.cpp
    src/ResourceGovernor.cpp
    src/PerformanceEngine.cpp
    src/WindowsPerformanceBackend.cpp
)

target_include_directories(
    SurfacePerformanceEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfacePerformanceEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    PerformanceTests

    tests/PerformanceTests.cpp
    src/PerformanceModel.cpp
    src/PerformancePolicy.cpp
    src/ResourceGovernor.cpp
)

target_include_directories(
    PerformanceTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME PerformanceTests
    COMMAND PerformanceTests
)







Project structure
SurfaceSecurityEngine/
├── CMakeLists.txt
├── include/
│   ├── SecurityTypes.hpp
│   ├── SecureKey.hpp
│   ├── Cryptography.hpp
│   ├── DeviceIdentity.hpp
│   ├── IntegrityMonitor.hpp
│   ├── SecurityPolicy.hpp
│   ├── AuditLog.hpp
│   ├── SecurityEngine.hpp
│   └── WindowsSecurityBackend.hpp
├── src/
│   ├── Cryptography.cpp
│   ├── DeviceIdentity.cpp
│   ├── IntegrityMonitor.cpp
│   ├── SecurityPolicy.cpp
│   ├── AuditLog.cpp
│   ├── SecurityEngine.cpp
│   ├── WindowsSecurityBackend.cpp
│   └── main.cpp
└── tests/
    └── SecurityTests.cpp
1. include/SecurityTypes.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace surface::security {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class SecurityLevel
{
    Normal,
    Elevated,
    Restricted,
    Critical
};

enum class TrustState
{
    Unknown,
    Trusted,
    Untrusted,
    Compromised
};

enum class KeyType
{
    Symmetric,
    RSA,
    ECC,
    DeviceIdentity,
    Session
};

enum class HashAlgorithm
{
    SHA256,
    SHA384,
    SHA512
};

enum class SecurityEventType
{
    Startup,
    Shutdown,
    Authentication,
    KeyCreated,
    KeyUsed,
    KeyDeleted,
    IntegrityCheck,
    PolicyChange,
    SecurityViolation,
    Encryption,
    Decryption
};

struct DeviceIdentity
{
    std::string deviceId;

    std::string manufacturer;

    std::string model;

    std::string firmwareVersion;

    TrustState trust{
        TrustState::Unknown
    };
};

struct SecureKey
{
    std::string identifier;

    KeyType type{
        KeyType::Session
    };

    std::uint32_t keySizeBits{0};

    bool hardwareBacked{false};

    bool exportable{false};

    bool persistent{false};
};

struct SecurityPolicy
{
    SecurityLevel level{
        SecurityLevel::Normal
    };

    bool requireHardwareBackedKeys{true};

    bool allowExportableKeys{false};

    bool requireDeviceTrust{true};

    bool enableIntegrityMonitoring{true};

    bool enableAuditLogging{true};

    bool requireSecureSessions{true};

    std::uint32_t minimumRSAKeyBits{2048};

    std::string reason;
};

struct IntegrityState
{
    bool secureBoot{
        false
    };

    bool trustedPlatformModule{
        false
    };

    bool codeIntegrity{
        false
    };

    bool deviceTrusted{
        false
    };

    TrustState overallTrust{
        TrustState::Unknown
    };
};

struct SecuritySnapshot
{
    DeviceIdentity identity;

    SecurityPolicy policy;

    IntegrityState integrity;

    SecurityLevel level{
        SecurityLevel::Normal
    };

    TimePoint timestamp{
        Clock::now()
    };
};

struct SecurityEvent
{
    SecurityEventType type;

    std::string description;

    TimePoint timestamp{
        Clock::now()
    };

    bool securityRelevant{true};
};

}
2. include/SecureKey.hpp
#pragma once

#include "SecurityTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace surface::security {

class IKeyStore
{
public:

    virtual ~IKeyStore() = default;

    virtual std::optional<SecureKey>
    createKey(
        KeyType type,
        std::uint32_t keySizeBits,
        bool hardwareBacked) = 0;

    virtual bool deleteKey(
        const std::string& identifier) = 0;

    virtual std::optional<SecureKey>
    describeKey(
        const std::string& identifier) const = 0;
};

}
3. include/Cryptography.hpp
#pragma once

#include "SecurityTypes.hpp"

#include <cstdint>
#include <vector>

namespace surface::security {

struct HashResult
{
    HashAlgorithm algorithm;

    std::vector<std::uint8_t>
        digest;
};

class Cryptography
{
public:

    static HashResult sha256(
        const std::vector<std::uint8_t>& data);

    static bool constantTimeEqual(
        const std::vector<std::uint8_t>& a,
        const std::vector<std::uint8_t>& b);
};

}
4. src/Cryptography.cpp
#include "Cryptography.hpp"

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#endif

namespace surface::security {

HashResult Cryptography::sha256(
    const std::vector<std::uint8_t>& data)
{
    HashResult result;

    result.algorithm =
        HashAlgorithm::SHA256;

#ifdef _WIN32

    BCRYPT_ALG_HANDLE algorithm = nullptr;

    BCRYPT_HASH_HANDLE hash = nullptr;

    NTSTATUS status =
        BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0
        );

    if (status < 0)
    {
        return result;
    }

    DWORD objectSize = 0;
    DWORD resultSize = 0;

    status =
        BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &resultSize,
            0
        );

    if (status < 0)
    {
        BCryptCloseAlgorithmProvider(
            algorithm,
            0
        );

        return result;
    }

    std::vector<UCHAR>
        object(objectSize);

    status =
        BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            objectSize,
            nullptr,
            0,
            0
        );

    if (status < 0)
    {
        BCryptCloseAlgorithmProvider(
            algorithm,
            0
        );

        return result;
    }

    status =
        BCryptHashData(
            hash,
            const_cast<PUCHAR>(
                reinterpret_cast<
                    const UCHAR*
                >(data.data())
            ),
            static_cast<ULONG>(
                data.size()
            ),
            0
        );

    if (status >= 0)
    {
        result.digest.resize(32);

        status =
            BCryptFinishHash(
                hash,
                result.digest.data(),
                static_cast<ULONG>(
                    result.digest.size()
                ),
                0
            );
    }

    BCryptDestroyHash(hash);

    BCryptCloseAlgorithmProvider(
        algorithm,
        0
    );

    if (status < 0)
    {
        result.digest.clear();
    }

#else

    /*
        No fallback cryptographic implementation is
        provided. Production code should use a vetted
        platform cryptographic library.
    */

    (void)data;

#endif

    return result;
}

bool Cryptography::constantTimeEqual(
    const std::vector<std::uint8_t>& a,
    const std::vector<std::uint8_t>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    std::uint8_t difference = 0;

    for (std::size_t i = 0;
         i < a.size();
         ++i)
    {
        difference |=
            a[i] ^ b[i];
    }

    return difference == 0;
}

}

This is deliberately using Windows CNG/BCrypt rather than implementing SHA-256 manually.

5. include/DeviceIdentity.hpp
#pragma once

#include "SecurityTypes.hpp"

namespace surface::security {

class IDeviceIdentityProvider
{
public:

    virtual ~IDeviceIdentityProvider() = default;

    virtual DeviceIdentity
    identity() const = 0;

    virtual bool verifyTrust() = 0;
};

}
6. src/DeviceIdentity.cpp
#include "DeviceIdentity.hpp"

namespace surface::security {

/*
    Platform-specific identity implementation lives
    in WindowsSecurityBackend.

    This file intentionally keeps the abstraction
    independent of Windows.
*/

}
7. include/IntegrityMonitor.hpp
#pragma once

#include "SecurityTypes.hpp"

namespace surface::security {

class IIntegrityProvider
{
public:

    virtual ~IIntegrityProvider() = default;

    virtual IntegrityState
    check() = 0;
};

class IntegrityMonitor
{
public:

    explicit IntegrityMonitor(
        IIntegrityProvider& provider);

    IntegrityState evaluate();

private:

    IIntegrityProvider&
        provider_;
};

}
8. src/IntegrityMonitor.cpp
#include "IntegrityMonitor.hpp"

namespace surface::security {

IntegrityMonitor::IntegrityMonitor(
    IIntegrityProvider& provider)
    : provider_(provider)
{
}

IntegrityState
IntegrityMonitor::evaluate()
{
    return provider_.check();
}

}
9. include/SecurityPolicy.hpp
#pragma once

#include "SecurityTypes.hpp"

namespace surface::security {

class SecurityPolicyEngine
{
public:

    SecurityPolicy calculate(
        const IntegrityState& integrity,
        const DeviceIdentity& identity) const;
};

}
10. src/SecurityPolicy.cpp
#include "SecurityPolicy.hpp"

namespace surface::security {

SecurityPolicy
SecurityPolicyEngine::calculate(
    const IntegrityState& integrity,
    const DeviceIdentity& identity) const
{
    SecurityPolicy policy;

    /*
        Secure baseline.
    */

    policy.level =
        SecurityLevel::Normal;

    policy.requireHardwareBackedKeys =
        true;

    policy.allowExportableKeys =
        false;

    policy.requireDeviceTrust =
        true;

    policy.enableIntegrityMonitoring =
        true;

    policy.enableAuditLogging =
        true;

    policy.requireSecureSessions =
        true;

    /*
        If the platform reports that trust has failed,
        move to restricted operation.
    */

    if (
        integrity.overallTrust ==
            TrustState::Untrusted ||
        identity.trust ==
            TrustState::Untrusted)
    {
        policy.level =
            SecurityLevel::Restricted;

        policy.allowExportableKeys =
            false;

        policy.reason =
            "Device trust not established";
    }

    /*
        Compromised state gets the strictest policy.
    */

    if (
        integrity.overallTrust ==
            TrustState::Compromised ||
        identity.trust ==
            TrustState::Compromised)
    {
        policy.level =
            SecurityLevel::Critical;

        policy.requireHardwareBackedKeys =
            true;

        policy.allowExportableKeys =
            false;

        policy.requireDeviceTrust =
            true;

        policy.enableIntegrityMonitoring =
            true;

        policy.reason =
            "Integrity or device trust failure";
    }

    /*
        Missing hardware security capability.
    */

    if (!integrity.trustedPlatformModule)
    {
        policy.level =
            SecurityLevel::Elevated;

        policy.requireHardwareBackedKeys =
            true;

        policy.reason +=
            " Hardware-backed trust unavailable; ";
    }

    return policy;
}

}
11. include/AuditLog.hpp
#pragma once

#include "SecurityTypes.hpp"

#include <mutex>
#include <vector>

namespace surface::security {

class AuditLog
{
public:

    void record(
        SecurityEvent event);

    std::vector<SecurityEvent>
    events() const;

    void clear();

private:

    mutable std::mutex mutex_;

    std::vector<SecurityEvent>
        events_;
};

}
12. src/AuditLog.cpp
#include "AuditLog.hpp"

namespace surface::security {

void AuditLog::record(
    SecurityEvent event)
{
    std::lock_guard lock(mutex_);

    events_.push_back(
        std::move(event)
    );
}

std::vector<SecurityEvent>
AuditLog::events() const
{
    std::lock_guard lock(mutex_);

    return events_;
}

void AuditLog::clear()
{
    std::lock_guard lock(mutex_);

    events_.clear();
}

}
13. include/WindowsSecurityBackend.hpp
#pragma once

#include "DeviceIdentity.hpp"
#include "IntegrityMonitor.hpp"
#include "SecureKey.hpp"

namespace surface::security {

class WindowsSecurityBackend final
    : public IDeviceIdentityProvider,
      public IIntegrityProvider,
      public IKeyStore
{
public:

    DeviceIdentity
    identity() const override;

    bool verifyTrust() override;

    IntegrityState
    check() override;

    std::optional<SecureKey>
    createKey(
        KeyType type,
        std::uint32_t keySizeBits,
        bool hardwareBacked) override;

    bool deleteKey(
        const std::string& identifier) override;

    std::optional<SecureKey>
    describeKey(
        const std::string& identifier) const override;

private:

    DeviceIdentity identity_;

    IntegrityState integrity_;
};

}
14. src/WindowsSecurityBackend.cpp
#include "WindowsSecurityBackend.hpp"

#include <unordered_map>

namespace surface::security {

namespace {

std::unordered_map<
    std::string,
    SecureKey
> g_keys;

}

DeviceIdentity
WindowsSecurityBackend::identity() const
{
    /*
        Production implementation should obtain
        appropriate device identity information through
        supported Windows/platform facilities.

        Do not use an arbitrary MAC address or serial
        number as a cryptographic identity.
    */

    DeviceIdentity result;

    result.manufacturer =
        "Microsoft";

    result.model =
        "Surface";

    result.trust =
        TrustState::Unknown;

    return result;
}

bool
WindowsSecurityBackend::verifyTrust()
{
    integrity_ =
        check();

    identity_ =
        identity();

    identity_.trust =
        integrity_.overallTrust;

    return
        identity_.trust ==
        TrustState::Trusted;
}

IntegrityState
WindowsSecurityBackend::check()
{
    IntegrityState state;

    /*
        These values are conservative defaults.

        A production implementation should query the
        supported Windows security/platform interfaces
        for actual Secure Boot, TPM and code-integrity
        state rather than assuming that the machine is
        trusted.
    */

    state.secureBoot =
        false;

    state.trustedPlatformModule =
        false;

    state.codeIntegrity =
        false;

    state.deviceTrusted =
        false;

    state.overallTrust =
        TrustState::Unknown;

    return state;
}

std::optional<SecureKey>
WindowsSecurityBackend::createKey(
    KeyType type,
    std::uint32_t keySizeBits,
    bool hardwareBacked)
{
    if (keySizeBits == 0)
    {
        return std::nullopt;
    }

    SecureKey key;

    key.identifier =
        "surface-key-" +
        std::to_string(
            g_keys.size() + 1
        );

    key.type =
        type;

    key.keySizeBits =
        keySizeBits;

    key.hardwareBacked =
        hardwareBacked;

    /*
        Keys are deliberately non-exportable.
    */

    key.exportable =
        false;

    key.persistent =
        hardwareBacked;

    g_keys[key.identifier] =
        key;

    return key;
}

bool
WindowsSecurityBackend::deleteKey(
    const std::string& identifier)
{
    return
        g_keys.erase(identifier) > 0;
}

std::optional<SecureKey>
WindowsSecurityBackend::describeKey(
    const std::string& identifier) const
{
    const auto it =
        g_keys.find(identifier);

    if (it == g_keys.end())
    {
        return std::nullopt;
    }

    return it->second;
}

}

Important: the g_keys implementation above is only a development abstraction. For a production Surface implementation, private keys should live in an OS/hardware-backed key store rather than in an ordinary C++ process.

15. include/SecurityEngine.hpp
#pragma once

#include "AuditLog.hpp"
#include "DeviceIdentity.hpp"
#include "IntegrityMonitor.hpp"
#include "SecurityPolicy.hpp"
#include "SecureKey.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace surface::security {

class SecurityEngine
{
public:

    using SnapshotCallback =
        std::function<void(
            const SecuritySnapshot&)>;

    SecurityEngine(
        IDeviceIdentityProvider& identityProvider,
        IIntegrityProvider& integrityProvider,
        IKeyStore& keyStore);

    ~SecurityEngine();

    void start();

    void stop();

    void update();

    std::optional<SecureKey>
    createDeviceKey(
        KeyType type,
        std::uint32_t keySizeBits);

    bool deleteKey(
        const std::string& identifier);

    SecuritySnapshot snapshot() const;

    std::vector<SecurityEvent>
    auditEvents() const;

    void setCallback(
        SnapshotCallback callback);

private:

    void loop();

    IDeviceIdentityProvider&
        identityProvider_;

    IntegrityMonitor
        integrityMonitor_;

    IKeyStore&
        keyStore_;

    SecurityPolicyEngine
        policyEngine_;

    AuditLog
        auditLog_;

    mutable std::mutex mutex_;

    SecuritySnapshot
        snapshot_;

    SnapshotCallback
        callback_;

    std::atomic<bool>
        running_{false};

    std::thread worker_;
};

}
16. src/SecurityEngine.cpp
#include "SecurityEngine.hpp"

#include <chrono>

namespace surface::security {

SecurityEngine::SecurityEngine(
    IDeviceIdentityProvider& identityProvider,
    IIntegrityProvider& integrityProvider,
    IKeyStore& keyStore)
    : identityProvider_(identityProvider),
      integrityMonitor_(integrityProvider),
      keyStore_(keyStore)
{
}

SecurityEngine::~SecurityEngine()
{
    stop();
}

void SecurityEngine::start()
{
    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true))
    {
        return;
    }

    auditLog_.record(
        {
            SecurityEventType::Startup,
            "Security engine started",
            Clock::now(),
            true
        }
    );

    worker_ =
        std::thread(
            &SecurityEngine::loop,
            this
        );
}

void SecurityEngine::stop()
{
    bool expected = true;

    if (!running_.compare_exchange_strong(
            expected,
            false))
    {
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }

    auditLog_.record(
        {
            SecurityEventType::Shutdown,
            "Security engine stopped",
            Clock::now(),
            true
        }
    );
}

void SecurityEngine::update()
{
    const auto integrity =
        integrityMonitor_.evaluate();

    const auto identity =
        identityProvider_.identity();

    const auto policy =
        policyEngine_.calculate(
            integrity,
            identity
        );

    SecuritySnapshot snapshot;

    snapshot.identity =
        identity;

    snapshot.integrity =
        integrity;

    snapshot.policy =
        policy;

    snapshot.level =
        policy.level;

    snapshot.timestamp =
        Clock::now();

    auditLog_.record(
        {
            SecurityEventType::IntegrityCheck,
            "Platform integrity evaluated",
            Clock::now(),
            true
        }
    );

    SnapshotCallback callback;

    {
        std::lock_guard lock(mutex_);

        snapshot_ =
            snapshot;

        callback =
            callback_;
    }

    if (callback)
    {
        callback(snapshot);
    }
}

void SecurityEngine::loop()
{
    using namespace std::chrono_literals;

    while (running_)
    {
        update();

        /*
            Integrity checks are intentionally not run
            at kernel-level frequency. This is a policy
            monitor rather than the security boundary.
        */

        std::this_thread::sleep_for(
            5s
        );
    }
}

std::optional<SecureKey>
SecurityEngine::createDeviceKey(
    KeyType type,
    std::uint32_t keySizeBits)
{
    const auto current =
        snapshot();

    if (
        current.policy.level ==
        SecurityLevel::Critical)
    {
        auditLog_.record(
            {
                SecurityEventType::SecurityViolation,
                "Key creation blocked by critical security policy",
                Clock::now(),
                true
            }
        );

        return std::nullopt;
    }

    const auto key =
        keyStore_.createKey(
            type,
            keySizeBits,
            current.policy.requireHardwareBackedKeys
        );

    if (key)
    {
        auditLog_.record(
            {
                SecurityEventType::KeyCreated,
                "Secure key created",
                Clock::now(),
                true
            }
        );
    }

    return key;
}

bool SecurityEngine::deleteKey(
    const std::string& identifier)
{
    const bool deleted =
        keyStore_.deleteKey(
            identifier
        );

    auditLog_.record(
        {
            SecurityEventType::KeyDeleted,
            deleted
                ? "Secure key deleted"
                : "Secure key deletion failed",
            Clock::now(),
            true
        }
    );

    return deleted;
}

SecuritySnapshot
SecurityEngine::snapshot() const
{
    std::lock_guard lock(mutex_);

    return snapshot_;
}

std::vector<SecurityEvent>
SecurityEngine::auditEvents() const
{
    return auditLog_.events();
}

void SecurityEngine::setCallback(
    SnapshotCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}

}
17. src/main.cpp
#include "SecurityEngine.hpp"
#include "WindowsSecurityBackend.hpp"

#include <iostream>

using namespace surface::security;

int main()
{
    WindowsSecurityBackend backend;

    SecurityEngine engine(
        backend,
        backend,
        backend
    );

    engine.setCallback(
        [](const SecuritySnapshot& snapshot)
        {
            std::cout
                << "\n====================================\n"
                << " SURFACE SECURITY ENGINE\n"
                << "====================================\n"

                << "Manufacturer: "
                << snapshot.identity.manufacturer
                << "\n"

                << "Model: "
                << snapshot.identity.model
                << "\n"

                << "Security level: ";

            switch (snapshot.level)
            {
                case SecurityLevel::Normal:
                    std::cout << "NORMAL";
                    break;

                case SecurityLevel::Elevated:
                    std::cout << "ELEVATED";
                    break;

                case SecurityLevel::Restricted:
                    std::cout << "RESTRICTED";
                    break;

                case SecurityLevel::Critical:
                    std::cout << "CRITICAL";
                    break;
            }

            std::cout
                << "\n"

                << "Secure Boot: "
                << (
                    snapshot.integrity.secureBoot
                    ? "YES"
                    : "NO"
                )

                << "\nTPM available: "
                << (
                    snapshot.integrity.trustedPlatformModule
                    ? "YES"
                    : "NO"
                )

                << "\nCode integrity: "
                << (
                    snapshot.integrity.codeIntegrity
                    ? "YES"
                    : "NO"
                )

                << "\nDevice trusted: "
                << (
                    snapshot.integrity.deviceTrusted
                    ? "YES"
                    : "NO"
                )

                << "\nPolicy: "
                << snapshot.policy.reason
                << "\n";
        }
    );

    engine.start();

    /*
        Ask the security engine for a device key.

        In the production version this operation should
        map to the Windows-backed key storage provider.
    */

    const auto key =
        engine.createDeviceKey(
            KeyType::DeviceIdentity,
            256
        );

    if (key)
    {
        std::cout
            << "\nDevice key created:\n"
            << key->identifier
            << "\n"
            << "Hardware backed: "
            << (
                key->hardwareBacked
                    ? "YES"
                    : "NO"
            )
            << "\n";
    }
    else
    {
        std::cout
            << "\nDevice key creation was blocked "
            << "or unavailable.\n";
    }

    std::cout
        << "\nSurface Security Engine running.\n"
        << "Press ENTER to stop.\n";

    std::cin.get();

    engine.stop();

    return 0;
}
18. tests/SecurityTests.cpp
#include "Cryptography.hpp"
#include "SecurityPolicy.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace surface::security;

int main()
{
    /*
        Constant-time comparison.
    */

    std::vector<std::uint8_t>
        a =
        {
            1, 2, 3, 4
        };

    std::vector<std::uint8_t>
        b =
        {
            1, 2, 3, 4
        };

    std::vector<std::uint8_t>
        c =
        {
            1, 2, 3, 5
        };

    assert(
        Cryptography::constantTimeEqual(
            a,
            b
        )
    );

    assert(
        !Cryptography::constantTimeEqual(
            a,
            c
        )
    );

    /*
        SHA-256 should produce 32 bytes when
        Windows CNG is available.
    */

    const auto hash =
        Cryptography::sha256(a);

#ifdef _WIN32

    assert(
        hash.digest.size() == 32
    );

#endif

    /*
        Normal security policy.
    */

    SecurityPolicyEngine
        policyEngine;

    IntegrityState trustedIntegrity;

    trustedIntegrity.secureBoot =
        true;

    trustedIntegrity.trustedPlatformModule =
        true;

    trustedIntegrity.codeIntegrity =
        true;

    trustedIntegrity.deviceTrusted =
        true;

    trustedIntegrity.overallTrust =
        TrustState::Trusted;

    DeviceIdentity trustedIdentity;

    trustedIdentity.trust =
        TrustState::Trusted;

    auto policy =
        policyEngine.calculate(
            trustedIntegrity,
            trustedIdentity
        );

    assert(
        policy.level ==
        SecurityLevel::Normal
    );

    assert(
        policy.requireHardwareBackedKeys
    );

    /*
        Untrusted device.
    */

    IntegrityState untrustedIntegrity;

    untrustedIntegrity.overallTrust =
        TrustState::Untrusted;

    DeviceIdentity untrustedIdentity;

    untrustedIdentity.trust =
        TrustState::Untrusted;

    policy =
        policyEngine.calculate(
            untrustedIntegrity,
            untrustedIdentity
        );

    assert(
        policy.level ==
        SecurityLevel::Restricted
    );

    /*
        Compromised device.
    */

    IntegrityState compromisedIntegrity;

    compromisedIntegrity.overallTrust =
        TrustState::Compromised;

    DeviceIdentity compromisedIdentity;

    compromisedIdentity.trust =
        TrustState::Compromised;

    policy =
        policyEngine.calculate(
            compromisedIntegrity,
            compromisedIdentity
        );

    assert(
        policy.level ==
        SecurityLevel::Critical
    );

    assert(
        !policy.allowExportableKeys
    );

    std::cout
        << "All security tests passed.\n";

    return 0;
}
19. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceSecurityEngine
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD
    20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
)

add_executable(
    SurfaceSecurityEngine

    src/main.cpp
    src/Cryptography.cpp
    src/DeviceIdentity.cpp
    src/IntegrityMonitor.cpp
    src/SecurityPolicy.cpp
    src/AuditLog.cpp
    src/SecurityEngine.cpp
    src/WindowsSecurityBackend.cpp
)

target_include_directories(
    SurfaceSecurityEngine
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceSecurityEngine
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

    target_link_libraries(
        SurfaceSecurityEngine
        PRIVATE
            bcrypt
    )

endif()

enable_testing()

add_executable(
    SecurityTests

    tests/SecurityTests.cpp
    src/Cryptography.cpp
    src/SecurityPolicy.cpp
)

target_include_directories(
    SecurityTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(WIN32)

    target_compile_definitions(
        SecurityTests
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

    target_link_libraries(
        SecurityTests
        PRIVATE
            bcrypt
    )

endif()

add_test(
    NAME SecurityTests
    COMMAND SecurityTests
)



