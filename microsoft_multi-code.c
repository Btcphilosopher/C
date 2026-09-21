Surface Rust Security Service
SurfaceSecurityRust/
├── Cargo.toml
├── src/
│   ├── main.rs
│   ├── types.rs
│   ├── policy.rs
│   ├── identity.rs
│   ├── key_manager.rs
│   ├── integrity.rs
│   ├── audit.rs
│   ├── rate_limiter.rs
│   ├── service.rs
│   └── ipc.rs
└── tests/
    └── security_tests.rs
1. Cargo.toml
[package]
name = "surface-security-service"
version = "0.1.0"
edition = "2024"

[dependencies]
serde = { version = "1", features = ["derive"] }
serde_json = "1"
sha2 = "0.10"
thiserror = "2"
uuid = { version = "1", features = ["v4", "serde"] }
zeroize = "1"

[dev-dependencies]
2. src/types.rs
use serde::{Deserialize, Serialize};
use std::time::SystemTime;
use uuid::Uuid;

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Serialize,
    Deserialize,
)]
pub enum SecurityLevel {
    Normal,
    Elevated,
    Restricted,
    Critical,
}

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Serialize,
    Deserialize,
)]
pub enum TrustState {
    Unknown,
    Trusted,
    Untrusted,
    Compromised,
}

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Serialize,
    Deserialize,
)]
pub enum KeyType {
    DeviceIdentity,
    Session,
    Signing,
    Encryption,
}

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub struct DeviceIdentity {
    pub device_id: Uuid,
    pub manufacturer: String,
    pub model: String,
    pub trust: TrustState,
}

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub struct IntegrityState {
    pub secure_boot: bool,
    pub tpm_available: bool,
    pub code_integrity: bool,
    pub trusted_device: bool,
    pub overall_trust: TrustState,
}

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub struct SecurityPolicy {
    pub level: SecurityLevel,
    pub require_hardware_backed_keys: bool,
    pub allow_key_export: bool,
    pub require_device_trust: bool,
    pub enable_integrity_monitoring: bool,
    pub enable_audit: bool,
}

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub struct KeyMetadata {
    pub id: Uuid,
    pub key_type: KeyType,
    pub bits: u32,
    pub hardware_backed: bool,
    pub exportable: bool,
}

#[derive(Debug, Clone)]
pub struct SecurityEvent {
    pub timestamp: SystemTime,
    pub description: String,
    pub security_relevant: bool,
}

#[derive(Debug, Clone)]
pub struct SecuritySnapshot {
    pub identity: DeviceIdentity,
    pub integrity: IntegrityState,
    pub policy: SecurityPolicy,
}
3. src/policy.rs
use crate::types::{
    DeviceIdentity,
    IntegrityState,
    SecurityLevel,
    SecurityPolicy,
    TrustState,
};

pub struct SecurityPolicyEngine;

impl SecurityPolicyEngine {
    pub fn calculate(
        identity: &DeviceIdentity,
        integrity: &IntegrityState,
    ) -> SecurityPolicy {
        let mut policy = SecurityPolicy {
            level: SecurityLevel::Normal,
            require_hardware_backed_keys: true,
            allow_key_export: false,
            require_device_trust: true,
            enable_integrity_monitoring: true,
            enable_audit: true,
        };

        if identity.trust == TrustState::Untrusted
            || integrity.overall_trust == TrustState::Untrusted
        {
            policy.level = SecurityLevel::Restricted;
            policy.allow_key_export = false;
        }

        if identity.trust == TrustState::Compromised
            || integrity.overall_trust == TrustState::Compromised
        {
            policy.level = SecurityLevel::Critical;
            policy.require_hardware_backed_keys = true;
            policy.allow_key_export = false;
        }

        if !integrity.tpm_available
            && policy.level == SecurityLevel::Normal
        {
            policy.level = SecurityLevel::Elevated;
        }

        policy
    }
}
4. src/identity.rs
use crate::types::{
    DeviceIdentity,
    TrustState,
};
use uuid::Uuid;

pub trait IdentityProvider {
    fn identity(&self) -> DeviceIdentity;
    fn verify_trust(&self) -> TrustState;
}

pub struct WindowsIdentityProvider {
    identity: DeviceIdentity,
}

impl WindowsIdentityProvider {
    pub fn new() -> Self {
        Self {
            identity: DeviceIdentity {
                device_id: Uuid::new_v4(),
                manufacturer: "Microsoft".to_string(),
                model: "Surface".to_string(),
                trust: TrustState::Unknown,
            },
        }
    }
}

impl IdentityProvider for WindowsIdentityProvider {
    fn identity(&self) -> DeviceIdentity {
        self.identity.clone()
    }

    fn verify_trust(&self) -> TrustState {
        /*
         * Production implementation:
         *
         * - query supported Windows security APIs
         * - verify platform trust
         * - inspect TPM-backed state
         * - obtain appropriate device identity
         *
         * Never treat a random UUID as a hardware identity.
         */

        TrustState::Unknown
    }
}
5. src/integrity.rs
use crate::types::{
    IntegrityState,
    TrustState,
};

pub trait IntegrityProvider {
    fn check_integrity(&self) -> IntegrityState;
}

pub struct WindowsIntegrityProvider;

impl WindowsIntegrityProvider {
    pub fn new() -> Self {
        Self
    }
}

impl IntegrityProvider for WindowsIntegrityProvider {
    fn check_integrity(&self) -> IntegrityState {
        /*
         * Conservative defaults.
         *
         * The actual Windows implementation should query
         * documented platform security facilities.
         */

        IntegrityState {
            secure_boot: false,
            tpm_available: false,
            code_integrity: false,
            trusted_device: false,
            overall_trust: TrustState::Unknown,
        }
    }
}
6. src/key_manager.rs

This layer is deliberately designed so that Rust doesn't implement its own TPM or invent a home-made secure enclave.

use crate::types::{
    KeyMetadata,
    KeyType,
    SecurityLevel,
};

use std::collections::HashMap;
use thiserror::Error;
use uuid::Uuid;

#[derive(Debug, Error)]
pub enum KeyError {
    #[error("key creation blocked by security policy")]
    PolicyBlocked,

    #[error("invalid key size")]
    InvalidKeySize,

    #[error("key not found")]
    NotFound,
}

pub struct KeyManager {
    keys: HashMap<Uuid, KeyMetadata>,
}

impl KeyManager {
    pub fn new() -> Self {
        Self {
            keys: HashMap::new(),
        }
    }

    pub fn create_key(
        &mut self,
        key_type: KeyType,
        bits: u32,
        hardware_backed: bool,
        level: SecurityLevel,
    ) -> Result<KeyMetadata, KeyError> {
        if bits < 256 {
            return Err(KeyError::InvalidKeySize);
        }

        if level == SecurityLevel::Critical
            && !hardware_backed
        {
            return Err(KeyError::PolicyBlocked);
        }

        let metadata = KeyMetadata {
            id: Uuid::new_v4(),
            key_type,
            bits,
            hardware_backed,
            exportable: false,
        };

        self.keys
            .insert(metadata.id, metadata.clone());

        Ok(metadata)
    }

    pub fn delete_key(
        &mut self,
        id: Uuid,
    ) -> Result<(), KeyError> {
        self.keys
            .remove(&id)
            .map(|_| ())
            .ok_or(KeyError::NotFound)
    }

    pub fn get(
        &self,
        id: Uuid,
    ) -> Option<&KeyMetadata> {
        self.keys.get(&id)
    }

    pub fn count(&self) -> usize {
        self.keys.len()
    }
}
7. src/audit.rs
use crate::types::SecurityEvent;

use std::sync::{
    Arc,
    Mutex,
};

use std::time::SystemTime;

#[derive(Clone)]
pub struct AuditLog {
    events: Arc<Mutex<Vec<SecurityEvent>>>,
}

impl AuditLog {
    pub fn new() -> Self {
        Self {
            events: Arc::new(
                Mutex::new(Vec::new())
            ),
        }
    }

    pub fn record(
        &self,
        description: impl Into<String>,
        security_relevant: bool,
    ) {
        let event = SecurityEvent {
            timestamp: SystemTime::now(),
            description: description.into(),
            security_relevant,
        };

        if let Ok(mut events) =
            self.events.lock()
        {
            events.push(event);
        }
    }

    pub fn len(&self) -> usize {
        self.events
            .lock()
            .map(|events| events.len())
            .unwrap_or(0)
    }

    pub fn security_events(
        &self,
    ) -> Vec<SecurityEvent> {
        self.events
            .lock()
            .map(|events| {
                events
                    .iter()
                    .filter(|e| e.security_relevant)
                    .cloned()
                    .collect()
            })
            .unwrap_or_default()
    }
}
8. src/rate_limiter.rs

This protects the security service itself from request flooding.

use std::time::{
    Duration,
    Instant,
};

pub struct RateLimiter {
    max_requests: u32,
    window: Duration,
    requests: Vec<Instant>,
}

impl RateLimiter {
    pub fn new(
        max_requests: u32,
        window: Duration,
    ) -> Self {
        Self {
            max_requests,
            window,
            requests: Vec::new(),
        }
    }

    pub fn allow(&mut self) -> bool {
        let now = Instant::now();

        self.requests.retain(|timestamp| {
            now.duration_since(*timestamp)
                < self.window
        });

        if self.requests.len()
            >= self.max_requests as usize
        {
            return false;
        }

        self.requests.push(now);

        true
    }

    pub fn current_count(&self) -> usize {
        self.requests.len()
    }
}
9. src/ipc.rs

A deliberately small message boundary between the Rust service and the other Surface components.

use serde::{
    Deserialize,
    Serialize,
};

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub enum SecurityRequest {
    GetSnapshot,

    VerifyDevice,

    CreateKey {
        bits: u32,
    },

    DeleteKey {
        id: String,
    },

    IntegrityCheck,

    Ping,
}

#[derive(
    Debug,
    Clone,
    Serialize,
    Deserialize,
)]
pub enum SecurityResponse {
    Pong,

    Accepted,

    Denied,

    Snapshot {
        security_level: String,
        trusted: bool,
    },

    KeyCreated {
        id: String,
    },

    Error {
        message: String,
    },
}
10. src/service.rs

This is the central Rust security service.

use crate::audit::AuditLog;
use crate::identity::IdentityProvider;
use crate::integrity::IntegrityProvider;
use crate::key_manager::KeyManager;
use crate::policy::SecurityPolicyEngine;
use crate::types::{
    SecurityLevel,
    SecuritySnapshot,
};

use crate::ipc::{
    SecurityRequest,
    SecurityResponse,
};

use crate::rate_limiter::RateLimiter;

use std::sync::{
    Arc,
    Mutex,
};

use std::time::Duration;

pub struct SecurityService<
    I,
    T,
> where
    I: IdentityProvider,
    T: IntegrityProvider,
{
    identity_provider: I,
    integrity_provider: T,

    key_manager: Arc<Mutex<KeyManager>>,

    audit: AuditLog,

    rate_limiter:
        Arc<Mutex<RateLimiter>>,

    snapshot:
        Arc<Mutex<Option<SecuritySnapshot>>>,
}

impl<I, T> SecurityService<I, T>
where
    I: IdentityProvider,
    T: IntegrityProvider,
{
    pub fn new(
        identity_provider: I,
        integrity_provider: T,
    ) -> Self {
        Self {
            identity_provider,
            integrity_provider,

            key_manager:
                Arc::new(
                    Mutex::new(
                        KeyManager::new()
                    )
                ),

            audit:
                AuditLog::new(),

            rate_limiter:
                Arc::new(
                    Mutex::new(
                        RateLimiter::new(
                            100,
                            Duration::from_secs(1),
                        )
                    )
                ),

            snapshot:
                Arc::new(
                    Mutex::new(None)
                ),
        }
    }

    pub fn update(&self) {
        let mut identity =
            self.identity_provider.identity();

        identity.trust =
            self.identity_provider.verify_trust();

        let integrity =
            self.integrity_provider
                .check_integrity();

        let policy =
            SecurityPolicyEngine::calculate(
                &identity,
                &integrity,
            );

        let snapshot =
            SecuritySnapshot {
                identity,
                integrity,
                policy,
            };

        if let Ok(mut current) =
            self.snapshot.lock()
        {
            *current =
                Some(snapshot);
        }

        self.audit.record(
            "Security state evaluated",
            true,
        );
    }

    pub fn snapshot(
        &self,
    ) -> Option<SecuritySnapshot> {
        self.snapshot
            .lock()
            .ok()
            .and_then(|snapshot| snapshot.clone())
    }

    pub fn handle(
        &self,
        request: SecurityRequest,
    ) -> SecurityResponse {
        let allowed =
            self.rate_limiter
                .lock()
                .map(|mut limiter| limiter.allow())
                .unwrap_or(false);

        if !allowed {
            self.audit.record(
                "Security request rate limit exceeded",
                true,
            );

            return SecurityResponse::Denied;
        }

        match request {
            SecurityRequest::Ping => {
                SecurityResponse::Pong
            }

            SecurityRequest::GetSnapshot => {
                match self.snapshot() {
                    Some(snapshot) => {
                        SecurityResponse::Snapshot {
                            security_level:
                                format!(
                                    "{:?}",
                                    snapshot.policy.level
                                ),

                            trusted:
                                snapshot.integrity
                                    .trusted_device,
                        }
                    }

                    None => {
                        SecurityResponse::Error {
                            message:
                                "Security state unavailable"
                                    .into(),
                        }
                    }
                }
            }

            SecurityRequest::VerifyDevice => {
                self.update();

                match self.snapshot() {
                    Some(snapshot)
                        if snapshot.integrity
                            .trusted_device =>
                    {
                        SecurityResponse::Accepted
                    }

                    _ => {
                        SecurityResponse::Denied
                    }
                }
            }

            SecurityRequest::IntegrityCheck => {
                self.update();

                SecurityResponse::Accepted
            }

            SecurityRequest::CreateKey {
                bits,
            } => {
                let Some(snapshot) =
                    self.snapshot()
                else {
                    return SecurityResponse::Denied;
                };

                if snapshot.policy.level
                    == SecurityLevel::Critical
                {
                    self.audit.record(
                        "Key creation denied by critical policy",
                        true,
                    );

                    return SecurityResponse::Denied;
                }

                let result =
                    self.key_manager
                        .lock()
                        .map_err(|_| ())
                        .and_then(|mut manager| {
                            manager
                                .create_key(
                                    crate::types::KeyType::DeviceIdentity,
                                    bits,
                                    snapshot
                                        .policy
                                        .require_hardware_backed_keys,
                                    snapshot.policy.level,
                                )
                                .map_err(|_| ())
                        });

                match result {
                    Ok(key) => {
                        self.audit.record(
                            "Device key created",
                            true,
                        );

                        SecurityResponse::KeyCreated {
                            id: key.id.to_string(),
                        }
                    }

                    Err(_) => {
                        SecurityResponse::Denied
                    }
                }
            }

            SecurityRequest::DeleteKey {
                id,
            } => {
                let Ok(uuid) =
                    uuid::Uuid::parse_str(&id)
                else {
                    return SecurityResponse::Error {
                        message:
                            "Invalid key identifier"
                                .into(),
                    };
                };

                let deleted =
                    self.key_manager
                        .lock()
                        .map(|mut manager| {
                            manager
                                .delete_key(uuid)
                                .is_ok()
                        })
                        .unwrap_or(false);

                if deleted {
                    self.audit.record(
                        "Key deleted",
                        true,
                    );

                    SecurityResponse::Accepted
                } else {
                    SecurityResponse::Denied
                }
            }
        }
    }

    pub fn audit_log(
        &self,
    ) -> AuditLog {
        self.audit.clone()
    }
}
11. src/main.rs
mod audit;
mod identity;
mod integrity;
mod ipc;
mod key_manager;
mod policy;
mod rate_limiter;
mod service;
mod types;

use identity::WindowsIdentityProvider;
use integrity::WindowsIntegrityProvider;
use ipc::SecurityRequest;
use service::SecurityService;

fn main() {
    println!(
        "=========================================="
    );

    println!(
        "       SURFACE RUST SECURITY SERVICE"
    );

    println!(
        "=========================================="
    );

    let identity =
        WindowsIdentityProvider::new();

    let integrity =
        WindowsIntegrityProvider::new();

    let service =
        SecurityService::new(
            identity,
            integrity,
        );

    /*
     * Initial security evaluation.
     */

    service.update();

    if let Some(snapshot) =
        service.snapshot()
    {
        println!(
            "Device: {}",
            snapshot.identity.model
        );

        println!(
            "Security level: {:?}",
            snapshot.policy.level
        );

        println!(
            "Trust: {:?}",
            snapshot.identity.trust
        );

        println!(
            "TPM available: {}",
            snapshot.integrity.tpm_available
        );

        println!(
            "Secure Boot: {}",
            snapshot.integrity.secure_boot
        );
    }

    /*
     * Test IPC requests.
     */

    let response =
        service.handle(
            SecurityRequest::Ping
        );

    println!(
        "Ping response: {:?}",
        response
    );

    let response =
        service.handle(
            SecurityRequest::GetSnapshot
        );

    println!(
        "Security response: {:?}",
        response
    );

    let response =
        service.handle(
            SecurityRequest::CreateKey {
                bits: 256,
            }
        );

    println!(
        "Key response: {:?}",
        response
    );

    println!(
        "Audit events: {}",
        service.audit_log().len()
    );
}
12. tests/security_tests.rs
use std::time::Duration;

use surface_security_service::{
    key_manager::KeyManager,
    rate_limiter::RateLimiter,
    types::{
        KeyType,
        SecurityLevel,
    },
};

#[test]
fn key_creation_works() {
    let mut manager =
        KeyManager::new();

    let result =
        manager.create_key(
            KeyType::DeviceIdentity,
            256,
            true,
            SecurityLevel::Normal,
        );

    assert!(result.is_ok());

    assert_eq!(
        manager.count(),
        1
    );
}

#[test]
fn tiny_key_is_rejected() {
    let mut manager =
        KeyManager::new();

    let result =
        manager.create_key(
            KeyType::Encryption,
            128,
            true,
            SecurityLevel::Normal,
        );

    assert!(result.is_err());
}

#[test]
fn critical_policy_requires_hardware_key() {
    let mut manager =
        KeyManager::new();

    let result =
        manager.create_key(
            KeyType::DeviceIdentity,
            256,
            false,
            SecurityLevel::Critical,
        );

    assert!(result.is_err());
}

#[test]
fn rate_limiter_blocks_excessive_requests() {
    let mut limiter =
        RateLimiter::new(
            2,
            Duration::from_secs(10),
        );

    assert!(limiter.allow());
    assert!(limiter.allow());
    assert!(!limiter.allow());
}
One small Cargo change

Because the integration tests import the crate itself, expose the modules through src/lib.rs.

src/lib.rs
pub mod audit;
pub mod identity;
pub mod integrity;
pub mod ipc;
pub mod key_manager;
pub mod policy;
pub mod rate_limiter;
pub mod service;
pub mod types;













Surface Local AI Engine
SurfaceLocalAI/
├── CMakeLists.txt
├── requirements.txt
├── python/
│   ├── model_config.py
│   ├── preprocess.py
│   ├── benchmark.py
│   └── export_model.py
├── cpp/
│   ├── include/
│   │   ├── AITypes.hpp
│   │   ├── Tensor.hpp
│   │   ├── AIModel.hpp
│   │   ├── Preprocessor.hpp
│   │   ├── NPUBackend.hpp
│   │   ├── CPUBackend.hpp
│   │   ├── AIBackendSelector.hpp
│   │   ├── AIEngine.hpp
│   │   └── WindowsAIBackend.hpp
│   └── src/
│       ├── Tensor.cpp
│       ├── Preprocessor.cpp
│       ├── NPUBackend.cpp
│       ├── CPUBackend.cpp
│       ├── AIBackendSelector.cpp
│       ├── AIEngine.cpp
│       ├── WindowsAIBackend.cpp
│       └── main.cpp
└── tests/
    └── AIEngineTests.cpp
PART I — Python
1. python/model_config.py
from dataclasses import dataclass
from enum import Enum


class Precision(Enum):
    FP32 = "fp32"
    FP16 = "fp16"
    INT8 = "int8"


@dataclass(frozen=True)
class ModelConfig:
    name: str
    input_width: int
    input_height: int
    channels: int
    precision: Precision
    max_batch_size: int = 1


SURFACE_VISION = ModelConfig(
    name="surface_vision",
    input_width=224,
    input_height=224,
    channels=3,
    precision=Precision.FP16,
)


SURFACE_SMALL_LLM = ModelConfig(
    name="surface_local_language",
    input_width=1,
    input_height=1,
    channels=1,
    precision=Precision.INT8,
)
2. python/preprocess.py

This is the Python-side model preprocessing layer.

from __future__ import annotations

import numpy as np


def normalize_image(
    image: np.ndarray,
) -> np.ndarray:
    """
    Convert uint8 image data into normalized
    floating-point model input.
    """

    if image.dtype != np.uint8:
        raise ValueError(
            "Expected uint8 image"
        )

    result = image.astype(
        np.float32
    ) / 255.0

    return result


def center_crop(
    image: np.ndarray,
    size: int,
) -> np.ndarray:
    height, width = image.shape[:2]

    if height < size or width < size:
        raise ValueError(
            "Image is smaller than crop"
        )

    top = (height - size) // 2
    left = (width - size) // 2

    return image[
        top:top + size,
        left:left + size
    ]


def prepare_image(
    image: np.ndarray,
    size: int = 224,
) -> np.ndarray:

    cropped = center_crop(
        image,
        size,
    )

    normalized = normalize_image(
        cropped
    )

    /*
        HWC -> CHW
    */

    return np.transpose(
        normalized,
        (2, 0, 1),
    )

Use Python here because experimentation with image/model preprocessing is dramatically easier than putting the whole research workflow into C++.

3. python/export_model.py

The Python layer should eventually export into a model/runtime format that the C++ production runtime understands.

from pathlib import Path


def export_model(
    source_model: str,
    output_path: str,
) -> None:

    source = Path(source_model)
    output = Path(output_path)

    if not source.exists():
        raise FileNotFoundError(
            source_model
        )

    output.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    /*
        This is intentionally a placeholder.

        A production implementation would use the
        framework's supported export mechanism, such
        as ONNX or another runtime-specific format.
    */

    output.write_bytes(
        source.read_bytes()
    )


if __name__ == "__main__":
    print(
        "Model export utility ready."
    )
4. python/benchmark.py

Now Python can become the AI laboratory for the Surface.

from dataclasses import dataclass
import statistics
import time


@dataclass
class BenchmarkResult:
    backend: str
    samples: int
    average_ms: float
    p95_ms: float


def percentile(
    values: list[float],
    percentile_value: float,
) -> float:

    values = sorted(values)

    if not values:
        return 0.0

    index = int(
        len(values)
        * percentile_value
    )

    index = min(
        index,
        len(values) - 1,
    )

    return values[index]


def benchmark(
    inference,
    samples: int = 100,
) -> BenchmarkResult:

    timings = []

    for _ in range(samples):

        start = time.perf_counter()

        inference()

        elapsed = (
            time.perf_counter()
            - start
        )

        timings.append(
            elapsed * 1000
        )

    return BenchmarkResult(
        backend="unknown",
        samples=samples,
        average_ms=statistics.mean(
            timings
        ),
        p95_ms=percentile(
            timings,
            0.95,
        ),
    )
PART II — C++ production runtime
5. cpp/include/AITypes.hpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace surface::ai {

enum class ComputeBackend
{
    CPU,
    GPU,
    NPU
};

enum class ModelPrecision
{
    FP32,
    FP16,
    INT8
};

enum class WorkloadType
{
    Vision,
    Speech,
    Language,
    Camera,
    Audio,
    GenerativeAI,
    Background
};

struct TensorShape
{
    std::vector<std::size_t> dimensions;

    std::size_t elementCount() const
    {
        std::size_t count = 1;

        for (const auto dimension : dimensions)
        {
            count *= dimension;
        }

        return count;
    }
};

struct ModelDescription
{
    std::string name;

    TensorShape inputShape;

    TensorShape outputShape;

    ModelPrecision precision{
        ModelPrecision::FP16
    };

    WorkloadType workload{
        WorkloadType::Vision
    };
};

struct AIHardwareState
{
    bool cpuAvailable{true};

    bool gpuAvailable{true};

    bool npuAvailable{false};

    double cpuLoad{0.0};

    double gpuLoad{0.0};

    double npuLoad{0.0};

    double temperatureC{40.0};

    double batteryPercent{100.0};

    bool onAC{true};
};

struct InferenceRequest
{
    std::string modelName;

    WorkloadType workload{
        WorkloadType::Vision
    };

    std::vector<float> input;

    bool latencyCritical{false};

    bool preferNPU{true};
};

struct InferenceResult
{
    ComputeBackend backend{
        ComputeBackend::CPU
    };

    std::vector<float> output;

    double inferenceMilliseconds{0.0};

    bool success{false};

    std::string error;
};

}
6. cpp/include/Tensor.hpp
#pragma once

#include "AITypes.hpp"

#include <cstddef>
#include <vector>

namespace surface::ai {

class Tensor
{
public:

    explicit Tensor(
        TensorShape shape
    );

    Tensor(
        TensorShape shape,
        std::vector<float> data
    );

    const TensorShape&
    shape() const;

    const std::vector<float>&
    data() const;

    std::vector<float>&
    data();

    std::size_t size() const;

private:

    TensorShape shape_;

    std::vector<float> data_;
};

}
7. cpp/src/Tensor.cpp
#include "Tensor.hpp"

#include <stdexcept>

namespace surface::ai {

Tensor::Tensor(
    TensorShape shape)
    : shape_(std::move(shape)),
      data_(shape_.elementCount())
{
}

Tensor::Tensor(
    TensorShape shape,
    std::vector<float> data)
    : shape_(std::move(shape)),
      data_(std::move(data))
{
    if (
        data_.size()
        != shape_.elementCount()
    )
    {
        throw std::invalid_argument(
            "Tensor data does not match shape"
        );
    }
}

const TensorShape&
Tensor::shape() const
{
    return shape_;
}

const std::vector<float>&
Tensor::data() const
{
    return data_;
}

std::vector<float>&
Tensor::data()
{
    return data_;
}

std::size_t
Tensor::size() const
{
    return data_.size();
}

}
8. cpp/include/Preprocessor.hpp
#pragma once

#include "Tensor.hpp"

namespace surface::ai {

class Preprocessor
{
public:

    Tensor normalize(
        const Tensor& input
    ) const;

    Tensor clamp(
        const Tensor& input,
        float minimum,
        float maximum
    ) const;
};

}
9. cpp/src/Preprocessor.cpp
#include "Preprocessor.hpp"

#include <algorithm>

namespace surface::ai {

Tensor
Preprocessor::normalize(
    const Tensor& input) const
{
    Tensor output(
        input.shape()
    );

    auto& values =
        output.data();

    const auto& source =
        input.data();

    for (
        std::size_t i = 0;
        i < source.size();
        ++i
    )
    {
        values[i] =
            source[i] / 255.0f;
    }

    return output;
}

Tensor
Preprocessor::clamp(
    const Tensor& input,
    float minimum,
    float maximum) const
{
    Tensor output(
        input.shape()
    );

    auto& values =
        output.data();

    const auto& source =
        input.data();

    for (
        std::size_t i = 0;
        i < source.size();
        ++i
    )
    {
        values[i] =
            std::clamp(
                source[i],
                minimum,
                maximum
            );
    }

    return output;
}

}
10. cpp/include/AIModel.hpp
#pragma once

#include "AITypes.hpp"
#include "Tensor.hpp"

#include <string>

namespace surface::ai {

class AIModel
{
public:

    explicit AIModel(
        ModelDescription description
    );

    const ModelDescription&
    description() const;

    bool validateInput(
        const Tensor& input
    ) const;

private:

    ModelDescription description_;
};

}
11. cpp/src/AIModel.cpp
#include "AIModel.hpp"

namespace surface::ai {

AIModel::AIModel(
    ModelDescription description)
    : description_(
        std::move(description))
{
}

const ModelDescription&
AIModel::description() const
{
    return description_;
}

bool AIModel::validateInput(
    const Tensor& input) const
{
    return
        input.shape().dimensions
        ==
        description_
            .inputShape
            .dimensions;
}

}
12. cpp/include/NPUBackend.hpp
#pragma once

#include "AIModel.hpp"

namespace surface::ai {

class INPUBackend
{
public:

    virtual ~INPUBackend() = default;

    virtual bool available() const = 0;

    virtual InferenceResult
    infer(
        const AIModel& model,
        const Tensor& input
    ) = 0;
};

class SurfaceNPUBackend final
    : public INPUBackend
{
public:

    bool available() const override;

    InferenceResult
    infer(
        const AIModel& model,
        const Tensor& input
    ) override;
};

}
13. cpp/src/NPUBackend.cpp
#include "NPUBackend.hpp"

namespace surface::ai {

bool
SurfaceNPUBackend::available() const
{
    /*
        Production implementation should detect and
        initialise the actual NPU through the supported
        Windows/SoC AI runtime.

        Do not assume every Surface model has an NPU.
    */

    return false;
}

InferenceResult
SurfaceNPUBackend::infer(
    const AIModel& model,
    const Tensor& input)
{
    InferenceResult result;

    result.backend =
        ComputeBackend::NPU;

    if (!available())
    {
        result.error =
            "NPU unavailable";

        return result;
    }

    if (!model.validateInput(input))
    {
        result.error =
            "Invalid model input";

        return result;
    }

    /*
        Production implementation:
        submit the model and tensor to the supported
        NPU runtime here.
    */

    result.success = false;

    result.error =
        "NPU execution backend not connected";

    return result;
}

}
14. cpp/include/CPUBackend.hpp
#pragma once

#include "AIModel.hpp"

namespace surface::ai {

class CPUBackend
{
public:

    InferenceResult infer(
        const AIModel& model,
        const Tensor& input
    ) const;
};

}
15. cpp/src/CPUBackend.cpp
#include "CPUBackend.hpp"

#include <chrono>

namespace surface::ai {

InferenceResult
CPUBackend::infer(
    const AIModel& model,
    const Tensor& input) const
{
    using Clock =
        std::chrono::steady_clock;

    InferenceResult result;

    result.backend =
        ComputeBackend::CPU;

    if (!model.validateInput(input))
    {
        result.error =
            "Invalid model input";

        return result;
    }

    const auto start =
        Clock::now();

    /*
        This is deliberately NOT pretending to
        perform neural-network inference.

        A real implementation would call the chosen
        production inference runtime here.
    */

    result.output =
        input.data();

    const auto end =
        Clock::now();

    result.inferenceMilliseconds =
        std::chrono::duration<double,
                              std::milli>(
            end - start
        ).count();

    result.success =
        true;

    return result;
}

}
16. cpp/include/AIBackendSelector.hpp
#pragma once

#include "AITypes.hpp"

namespace surface::ai {

class AIBackendSelector
{
public:

    ComputeBackend select(
        const InferenceRequest& request,
        const AIHardwareState& hardware
    ) const;
};

}
17. cpp/src/AIBackendSelector.cpp
#include "AIBackendSelector.hpp"

namespace surface::ai {

ComputeBackend
AIBackendSelector::select(
    const InferenceRequest& request,
    const AIHardwareState& hardware) const
{
    /*
        NPU is preferred for suitable AI workloads
        when available.
    */

    if (
        request.preferNPU
        &&
        hardware.npuAvailable
        &&
        hardware.npuLoad < 90.0
    )
    {
        return ComputeBackend::NPU;
    }

    /*
        GPU fallback for workloads that benefit from
        massively parallel execution.
    */

    if (
        hardware.gpuAvailable
        &&
        (
            request.workload
                == WorkloadType::Vision
            ||
            request.workload
                == WorkloadType::GenerativeAI
            ||
            request.workload
                == WorkloadType::Camera
        )
        &&
        hardware.gpuLoad < 90.0
    )
    {
        return ComputeBackend::GPU;
    }

    /*
        CPU is the universal fallback.
    */

    return ComputeBackend::CPU;
}

}
18. cpp/include/AIEngine.hpp
#pragma once

#include "AIBackendSelector.hpp"
#include "AIModel.hpp"
#include "CPUBackend.hpp"
#include "NPUBackend.hpp"
#include "Preprocessor.hpp"

#include <unordered_map>
#include <mutex>

namespace surface::ai {

class AIEngine
{
public:

    AIEngine();

    void registerModel(
        AIModel model
    );

    void updateHardwareState(
        AIHardwareState state
    );

    InferenceResult
    infer(
        const InferenceRequest& request
    );

    AIHardwareState
    hardwareState() const;

private:

    std::unordered_map<
        std::string,
        AIModel
    > models_;

    AIHardwareState hardware_;

    AIBackendSelector
        selector_;

    CPUBackend
        cpu_;

    SurfaceNPUBackend
        npu_;

    Preprocessor
        preprocessor_;

    mutable std::mutex
        mutex_;
};

}
19. cpp/src/AIEngine.cpp
#include "AIEngine.hpp"

#include <chrono>

namespace surface::ai {

AIEngine::AIEngine()
{
    hardware_.cpuAvailable =
        true;

    hardware_.gpuAvailable =
        true;

    hardware_.npuAvailable =
        npu_.available();
}

void AIEngine::registerModel(
    AIModel model)
{
    std::lock_guard lock(
        mutex_
    );

    models_.insert_or_assign(
        model.description().name,
        std::move(model)
    );
}

void AIEngine::updateHardwareState(
    AIHardwareState state)
{
    std::lock_guard lock(
        mutex_
    );

    hardware_ =
        state;
}

AIHardwareState
AIEngine::hardwareState() const
{
    std::lock_guard lock(
        mutex_
    );

    return hardware_;
}

InferenceResult
AIEngine::infer(
    const InferenceRequest& request)
{
    AIHardwareState hardware;

    AIModel model(
        ModelDescription{
            request.modelName,
            {{1}},
            {{1}},
            ModelPrecision::FP16,
            request.workload
        }
    );

    {
        std::lock_guard lock(
            mutex_
        );

        hardware =
            hardware_;

        const auto it =
            models_.find(
                request.modelName
            );

        if (it != models_.end())
        {
            model =
                it->second;
        }
    }

    Tensor input(
        model.description()
            .inputShape,
        request.input
    );

    const auto backend =
        selector_.select(
            request,
            hardware
        );

    if (
        backend ==
        ComputeBackend::NPU
    )
    {
        auto result =
            npu_.infer(
                model,
                input
            );

        if (result.success)
        {
            return result;
        }

        /*
            Graceful fallback.
        */
    }

    if (
        backend ==
        ComputeBackend::GPU
    )
    {
        /*
            GPU backend would be inserted here.
        */
    }

    return cpu_.infer(
        model,
        input
    );
}

}
20. cpp/include/WindowsAIBackend.hpp

This is where the Surface-specific Windows implementation eventually connects.

#pragma once

#include "AITypes.hpp"

namespace surface::ai {

class WindowsAIBackend
{
public:

    AIHardwareState
    queryHardware() const;

    bool
    initialize();

    void
    shutdown();

private:

    bool initialized_{false};
};

}
21. cpp/src/WindowsAIBackend.cpp
#include "WindowsAIBackend.hpp"

namespace surface::ai {

bool
WindowsAIBackend::initialize()
{
    /*
        Production implementation should initialise
        the supported Windows AI acceleration stack
        available on the target Surface platform.

        The runtime should discover:
            CPU
            GPU
            NPU
            supported precisions
            memory limits
            accelerator availability
    */

    initialized_ =
        true;

    return true;
}

void
WindowsAIBackend::shutdown()
{
    initialized_ =
        false;
}

AIHardwareState
WindowsAIBackend::queryHardware() const
{
    AIHardwareState state;

    state.cpuAvailable =
        true;

    state.gpuAvailable =
        true;

    /*
        Don't claim NPU availability without actually
        discovering one.
    */

    state.npuAvailable =
        false;

    return state;
}

}
22. cpp/src/main.cpp
#include "AIEngine.hpp"
#include "WindowsAIBackend.hpp"

#include <iostream>

using namespace surface::ai;

int main()
{
    std::cout
        << "=====================================\n"
        << "     SURFACE LOCAL AI ENGINE\n"
        << "=====================================\n";

    WindowsAIBackend
        windowsBackend;

    windowsBackend.initialize();

    AIEngine engine;

    engine.updateHardwareState(
        windowsBackend.queryHardware()
    );

    ModelDescription modelDescription;

    modelDescription.name =
        "surface_vision";

    modelDescription.inputShape =
        TensorShape{
            {1, 3, 224, 224}
        };

    modelDescription.outputShape =
        TensorShape{
            {1, 1000}
        };

    modelDescription.precision =
        ModelPrecision::FP16;

    modelDescription.workload =
        WorkloadType::Vision;

    engine.registerModel(
        AIModel(
            modelDescription
        )
    );

    InferenceRequest request;

    request.modelName =
        "surface_vision";

    request.workload =
        WorkloadType::Vision;

    request.preferNPU =
        true;

    request.latencyCritical =
        true;

    request.input.resize(
        1 * 3 * 224 * 224,
        0.5f
    );

    const auto result =
        engine.infer(
            request
        );

    std::cout
        << "Backend: ";

    switch (result.backend)
    {
        case ComputeBackend::CPU:
            std::cout << "CPU";
            break;

        case ComputeBackend::GPU:
            std::cout << "GPU";
            break;

        case ComputeBackend::NPU:
            std::cout << "NPU";
            break;
    }

    std::cout
        << "\nSuccess: "
        << (result.success
            ? "YES"
            : "NO")
        << "\nInference time: "
        << result.inferenceMilliseconds
        << " ms\n";

    if (!result.error.empty())
    {
        std::cout
            << "Message: "
            << result.error
            << "\n";
    }

    windowsBackend.shutdown();

    return 0;
}
23. tests/AIEngineTests.cpp
#include "AIBackendSelector.hpp"
#include "AIEngine.hpp"
#include "Tensor.hpp"

#include <cassert>
#include <iostream>

using namespace surface::ai;

int main()
{
    AIBackendSelector selector;

    InferenceRequest request;

    request.workload =
        WorkloadType::Vision;

    request.preferNPU =
        true;

    AIHardwareState hardware;

    hardware.cpuAvailable =
        true;

    hardware.gpuAvailable =
        true;

    hardware.npuAvailable =
        true;

    hardware.npuLoad =
        20.0;

    auto backend =
        selector.select(
            request,
            hardware
        );

    assert(
        backend ==
        ComputeBackend::NPU
    );

    /*
        NPU saturated:
        GPU should become the next candidate.
    */

    hardware.npuLoad =
        95.0;

    backend =
        selector.select(
            request,
            hardware
        );

    assert(
        backend ==
        ComputeBackend::GPU
    );

    /*
        Everything unavailable except CPU.
    */

    hardware.gpuAvailable =
        false;

    backend =
        selector.select(
            request,
            hardware
        );

    assert(
        backend ==
        ComputeBackend::CPU
    );

    /*
        Tensor test.
    */

    Tensor tensor(
        TensorShape{
            {1, 3, 4, 4}
        }
    );

    assert(
        tensor.size() == 48
    );

    std::cout
        << "All Surface AI tests passed.\n";

    return 0;
}
24. CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceLocalAI
    VERSION 1.0.0
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD 20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED ON
)

set(
    CMAKE_CXX_EXTENSIONS OFF
)

add_executable(
    SurfaceLocalAI

    cpp/src/main.cpp
    cpp/src/Tensor.cpp
    cpp/src/Preprocessor.cpp
    cpp/src/AIModel.cpp
    cpp/src/NPUBackend.cpp
    cpp/src/CPUBackend.cpp
    cpp/src/AIBackendSelector.cpp
    cpp/src/AIEngine.cpp
    cpp/src/WindowsAIBackend.cpp
)

target_include_directories(
    SurfaceLocalAI
    PRIVATE
        cpp/include
)

if(WIN32)

    target_compile_definitions(
        SurfaceLocalAI
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
    )

endif()

enable_testing()

add_executable(
    AIEngineTests

    tests/AIEngineTests.cpp
    cpp/src/Tensor.cpp
    cpp/src/Preprocessor.cpp
    cpp/src/AIModel.cpp
    cpp/src/NPUBackend.cpp
    cpp/src/CPUBackend.cpp
    cpp/src/AIBackendSelector.cpp
    cpp/src/AIEngine.cpp
)

target_include_directories(
    AIEngineTests
    PRIVATE
        cpp/include
)

add_test(
    NAME AIEngineTests
    COMMAND AIEngineTests
)
25. requirements.txt
numpy>=2.0











Project
SurfaceJuliaOptimizer/
├── Project.toml
├── src/
│   ├── SurfaceJuliaOptimizer.jl
│   ├── types.jl
│   ├── objective.jl
│   ├── thermal.jl
│   ├── battery.jl
│   ├── performance.jl
│   ├── workloads.jl
│   ├── optimizer.jl
│   ├── policy.jl
│   └── api.jl
├── examples/
│   └── surface_demo.jl
└── test/
    └── runtests.jl
1. Project.toml
name = "SurfaceJuliaOptimizer"
uuid = "9c7e9c4b-6f42-4a4e-9d6e-surface001"
authors = ["Surface Systems"]
version = "0.1.0"

[deps]
JuMP = "4076af6c-e467-56ae-b986-b466b2749572"
HiGHS = "87dc4568-4c63-4d18-9f9c-2d6f7f8f8f01"
JSON3 = "0f8b85d8-1f1d-4b7e-9f5a-2a0f7b7c9a1d"

[compat]
julia = "1.10"
JuMP = "1"
HiGHS = "1"
JSON3 = "1"

The solver is useful here because we're ultimately solving constrained optimisation problems rather than writing another ordinary scheduler.

2. src/types.jl
@enum ComputeUnit CPU GPU NPU

@enum PowerMode BatterySaver Balanced Performance MaximumPerformance

@enum WorkloadType General Interactive Gaming
                    Creative AI Media Background

struct SurfaceTelemetry
    cpu_util::Float64
    gpu_util::Float64
    npu_util::Float64

    cpu_temperature::Float64
    gpu_temperature::Float64
    npu_temperature::Float64

    battery_percent::Float64
    battery_power_watts::Float64

    memory_util::Float64

    on_ac::Bool
end

struct Workload
    name::String
    workload_type::WorkloadType

    cpu_demand::Float64
    gpu_demand::Float64
    npu_demand::Float64

    minimum_performance::Float64
    latency_sensitivity::Float64
end

struct SurfaceLimits
    maximum_cpu_temperature::Float64
    maximum_gpu_temperature::Float64
    maximum_npu_temperature::Float64

    minimum_battery_percent::Float64

    maximum_power_watts::Float64

    maximum_cpu_utilization::Float64
    maximum_gpu_utilization::Float64
    maximum_npu_utilization::Float64
end

struct OptimizationWeights
    performance::Float64
    battery::Float64
    thermal::Float64
    latency::Float64
end

struct OptimizationResult
    cpu_target::Float64
    gpu_target::Float64
    npu_target::Float64

    cpu_frequency_factor::Float64
    gpu_frequency_factor::Float64
    npu_frequency_factor::Float64

    display_refresh::Int
    power_limit_watts::Float64

    predicted_temperature::Float64
    predicted_power::Float64

    objective_value::Float64
end
3. src/thermal.jl

This creates a simple predictive thermal model.

struct ThermalModel
    ambient_temperature::Float64

    cpu_coefficient::Float64
    gpu_coefficient::Float64
    npu_coefficient::Float64

    cooling_rate::Float64
end


function predict_temperature(
    model::ThermalModel,
    current_temperature::Float64,
    cpu_load::Float64,
    gpu_load::Float64,
    npu_load::Float64,
    seconds::Float64,
)
    heat_input =
        model.cpu_coefficient * cpu_load +
        model.gpu_coefficient * gpu_load +
        model.npu_coefficient * npu_load

    cooling =
        model.cooling_rate *
        (current_temperature - model.ambient_temperature)

    rate =
        heat_input - cooling

    predicted =
        current_temperature +
        rate * seconds

    return predicted
end


function thermal_headroom(
    temperature::Float64,
    limit::Float64,
)
    return max(
        0.0,
        limit - temperature,
    )
end


function thermal_factor(
    temperature::Float64,
    limit::Float64,
)
    headroom =
        thermal_headroom(
            temperature,
            limit,
        )

    return clamp(
        headroom / 25.0,
        0.0,
        1.0,
    )
end
4. src/battery.jl
function battery_factor(
    battery_percent::Float64,
    on_ac::Bool,
)
    if on_ac
        return 1.0
    end

    if battery_percent <= 5.0
        return 0.25
    elseif battery_percent <= 15.0
        return 0.40
    elseif battery_percent <= 30.0
        return 0.65
    elseif battery_percent <= 50.0
        return 0.80
    else
        return 1.0
    end
end


function recommended_power_limit(
    battery_percent::Float64,
    on_ac::Bool,
    maximum_power::Float64,
)
    if on_ac
        return maximum_power
    end

    if battery_percent <= 10.0
        return maximum_power * 0.45
    elseif battery_percent <= 20.0
        return maximum_power * 0.60
    elseif battery_percent <= 40.0
        return maximum_power * 0.75
    end

    return maximum_power * 0.90
end
5. src/performance.jl
function performance_score(
    cpu::Float64,
    gpu::Float64,
    npu::Float64,
)
    return (
        0.40 * cpu +
        0.35 * gpu +
        0.25 * npu
    )
end


function utilization_pressure(
    telemetry::SurfaceTelemetry,
)
    cpu_pressure =
        max(
            0.0,
            telemetry.cpu_util - 80.0,
        ) / 20.0

    gpu_pressure =
        max(
            0.0,
            telemetry.gpu_util - 80.0,
        ) / 20.0

    npu_pressure =
        max(
            0.0,
            telemetry.npu_util - 80.0,
        ) / 20.0

    return clamp(
        maximum([
            cpu_pressure,
            gpu_pressure,
            npu_pressure,
        ]),
        0.0,
        1.0,
    )
end
6. src/workloads.jl

Julia can determine which hardware should receive the workload.

function preferred_unit(
    workload::Workload,
)
    demands = (
        CPU => workload.cpu_demand,
        GPU => workload.gpu_demand,
        NPU => workload.npu_demand,
    )

    return findmax(
        demands,
    )[2]
end


function workload_pressure(
    workload::Workload,
)
    return maximum([
        workload.cpu_demand,
        workload.gpu_demand,
        workload.npu_demand,
    ])
end


function latency_priority(
    workload::Workload,
)
    return clamp(
        workload.latency_sensitivity,
        0.0,
        1.0,
    )
end
7. src/objective.jl

Here's the core mathematical objective.

We want to maximise performance while penalising excessive power, heat and latency.

function optimization_score(
    performance,
    power,
    thermal_penalty,
    latency_penalty,
    weights::OptimizationWeights,
)
    return (
        weights.performance * performance
        -
        weights.battery * power
        -
        weights.thermal * thermal_penalty
        -
        weights.latency * latency_penalty
    )
end


function thermal_penalty(
    predicted_temperature,
    maximum_temperature,
)
    excess =
        max(
            0.0,
            predicted_temperature -
            maximum_temperature,
        )

    return excess^2
end
8. src/optimizer.jl

This is the main optimisation engine.

using JuMP
using HiGHS


struct SurfaceOptimizer
    limits::SurfaceLimits
    weights::OptimizationWeights
    thermal_model::ThermalModel
end


function optimize_surface(
    optimizer::SurfaceOptimizer,
    telemetry::SurfaceTelemetry,
    workload::Workload,
)
    model = Model(
        HiGHS.Optimizer,
    )

    set_silent(model)

    @variable(
        model,
        0.0 <= cpu <= 1.0
    )

    @variable(
        model,
        0.0 <= gpu <= 1.0
    )

    @variable(
        model,
        0.0 <= npu <= 1.0
    )

    @variable(
        model,
        20.0 <= power <=
        optimizer.limits.maximum_power_watts
    )

    @variable(
        model,
        refresh >= 30
    )

    @variable(
        model,
        refresh <= 144
    )

    /*
        Hardware utilisation limits.
    */

    @constraint(
        model,
        cpu <=
        optimizer.limits.maximum_cpu_utilization /
        100.0
    )

    @constraint(
        model,
        gpu <=
        optimizer.limits.maximum_gpu_utilization /
        100.0
    )

    @constraint(
        model,
        npu <=
        optimizer.limits.maximum_npu_utilization /
        100.0
    )

    /*
        Minimum workload performance.
    */

    @constraint(
        model,
        cpu >=
        workload.cpu_demand *
        workload.minimum_performance
    )

    @constraint(
        model,
        gpu >=
        workload.gpu_demand *
        workload.minimum_performance
    )

    @constraint(
        model,
        npu >=
        workload.npu_demand *
        workload.minimum_performance
    )

    /*
        Approximate performance.
    */

    @expression(
        model,
        performance,
        0.40 * cpu +
        0.35 * gpu +
        0.25 * npu
    )

    /*
        Thermal approximation.
    */

    @expression(
        model,
        predicted_temperature,
        telemetry.cpu_temperature +
        20.0 * cpu +
        12.0 * gpu +
        10.0 * npu
    )

    @constraint(
        model,
        predicted_temperature <=
        optimizer.limits.maximum_cpu_temperature
    )

    /*
        Power-performance relationship.
    */

    @constraint(
        model,
        power >=
        15.0 +
        45.0 * cpu +
        55.0 * gpu +
        35.0 * npu
    )

    /*
        Penalise excessive power.
    */

    @objective(
        model,
        Max,
        optimizer.weights.performance *
        performance

        -

        optimizer.weights.battery *
        power

        -

        optimizer.weights.thermal *
        (
            predicted_temperature / 100.0
        )

        +

        optimizer.weights.latency *
        refresh / 144.0
    )

    optimize!(model)

    if termination_status(model) != MOI.OPTIMAL
        return nothing
    end

    cpu_value =
        value(cpu)

    gpu_value =
        value(gpu)

    npu_value =
        value(npu)

    power_value =
        value(power)

    predicted =
        value(
            predicted_temperature
        )

    refresh_value =
        round(
            Int,
            value(refresh),
        )

    return OptimizationResult(
        cpu_value,
        gpu_value,
        npu_value,

        cpu_value,
        gpu_value,
        npu_value,

        refresh_value,
        power_value,

        predicted,
        power_value,

        objective_value(model),
    )
end
9. src/policy.jl

Now convert the mathematical solution into something the C++ Surface system can actually consume.

function generate_policy(
    result::OptimizationResult,
    telemetry::SurfaceTelemetry,
)
    cpu =
        clamp(
            result.cpu_frequency_factor,
            0.0,
            1.0,
        )

    gpu =
        clamp(
            result.gpu_frequency_factor,
            0.0,
            1.0,
        )

    npu =
        clamp(
            result.npu_frequency_factor,
            0.0,
            1.0,
        )

    refresh =
        clamp(
            result.display_refresh,
            30,
            144,
        )

    /*
        Emergency thermal override.
    */

    if result.predicted_temperature >= 95.0
        cpu *= 0.65
        gpu *= 0.55
        npu *= 0.60

        refresh =
            min(refresh, 60)
    end

    /*
        Battery protection.
    */

    if !telemetry.on_ac &&
       telemetry.battery_percent < 15.0

        cpu *= 0.60
        gpu *= 0.45
        npu *= 0.50

        refresh =
            min(refresh, 60)
    end

    return (
        cpu = cpu,
        gpu = gpu,
        npu = npu,
        refresh = refresh,
        power_limit = result.power_limit_watts,
    )
end
10. src/api.jl

A lightweight interface for the C++ side.

using JSON3


function telemetry_from_dict(
    data,
)
    return SurfaceTelemetry(
        Float64(data.cpu_util),
        Float64(data.gpu_util),
        Float64(data.npu_util),

        Float64(data.cpu_temperature),
        Float64(data.gpu_temperature),
        Float64(data.npu_temperature),

        Float64(data.battery_percent),
        Float64(data.battery_power_watts),

        Float64(data.memory_util),

        Bool(data.on_ac),
    )
end


function workload_from_dict(
    data,
)
    return Workload(
        String(data.name),
        WorkloadType(
            Symbol(data.workload_type)
        ),

        Float64(data.cpu_demand),
        Float64(data.gpu_demand),
        Float64(data.npu_demand),

        Float64(data.minimum_performance),
        Float64(data.latency_sensitivity),
    )
end


function optimize_json(
    json_input::String,
)
    input =
        JSON3.read(json_input)

    telemetry =
        telemetry_from_dict(
            input.telemetry
        )

    workload =
        workload_from_dict(
            input.workload
        )

    limits =
        SurfaceLimits(
            95.0,
            95.0,
            95.0,

            5.0,

            65.0,

            95.0,
            95.0,
            95.0,
        )

    weights =
        OptimizationWeights(
            1.0,
            0.20,
            1.50,
            0.50,
        )

    thermal =
        ThermalModel(
            22.0,
            0.08,
            0.10,
            0.08,
            0.015,
        )

    optimizer =
        SurfaceOptimizer(
            limits,
            weights,
            thermal,
        )

    result =
        optimize_surface(
            optimizer,
            telemetry,
            workload,
        )

    if result === nothing
        return JSON3.write(
            Dict(
                "success" => false,
                "error" =>
                    "No feasible optimisation solution",
            )
        )
    end

    policy =
        generate_policy(
            result,
            telemetry,
        )

    return JSON3.write(
        Dict(
            "success" => true,

            "cpu" => policy.cpu,
            "gpu" => policy.gpu,
            "npu" => policy.npu,

            "display_refresh" =>
                policy.refresh,

            "power_limit_watts" =>
                policy.power_limit,
        )
    )
end
11. src/SurfaceJuliaOptimizer.jl
module SurfaceJuliaOptimizer

include("types.jl")
include("thermal.jl")
include("battery.jl")
include("performance.jl")
include("workloads.jl")
include("objective.jl")
include("optimizer.jl")
include("policy.jl")
include("api.jl")

export SurfaceTelemetry
export Workload
export SurfaceLimits
export OptimizationWeights
export ThermalModel
export SurfaceOptimizer
export OptimizationResult

export optimize_surface
export generate_policy
export optimize_json

end
12. Demo

examples/surface_demo.jl

using Pkg

Pkg.activate(
    joinpath(
        @__DIR__,
        "..",
    )
)

using SurfaceJuliaOptimizer


telemetry =
    SurfaceTelemetry(

        55.0,       # CPU utilisation
        42.0,       # GPU utilisation
        20.0,       # NPU utilisation

        61.0,       # CPU temperature
        57.0,       # GPU temperature
        52.0,       # NPU temperature

        72.0,       # battery %
        18.0,       # battery watts

        54.0,       # memory %

        false,      # AC
    )


workload =
    Workload(
        "Local AI Vision",
        AI,

        0.30,
        0.45,
        0.90,

        0.75,
        0.80,
    )


limits =
    SurfaceLimits(
        95.0,
        95.0,
        95.0,

        5.0,

        65.0,

        95.0,
        95.0,
        95.0,
    )


weights =
    OptimizationWeights(
        1.0,     # performance
        0.20,    # battery
        1.50,    # thermal
        0.50,    # latency
    )


thermal =
    ThermalModel(
        22.0,
        0.08,
        0.10,
        0.08,
        0.015,
    )


optimizer =
    SurfaceOptimizer(
        limits,
        weights,
        thermal,
    )


result =
    optimize_surface(
        optimizer,
        telemetry,
        workload,
    )


if result === nothing

    println(
        "No feasible optimisation solution."
    )

else

    println(
        "======================================"
    )

    println(
        "       SURFACE JULIA OPTIMIZER"
    )

    println(
        "======================================"
    )

    println(
        "CPU target: ",
        result.cpu_target
    )

    println(
        "GPU target: ",
        result.gpu_target
    )

    println(
        "NPU target: ",
        result.npu_target
    )

    println(
        "Display refresh: ",
        result.display_refresh,
        " Hz"
    )

    println(
        "Power limit: ",
        result.power_limit_watts,
        " W"
    )

    println(
        "Predicted temperature: ",
        result.predicted_temperature,
        " °C"
    )

    println(
        "Objective: ",
        result.objective_value
    )

end
13. Tests

test/runtests.jl

using Test

include(
    "../src/SurfaceJuliaOptimizer.jl"
)

using .SurfaceJuliaOptimizer


@testset "Surface Optimizer" begin

    telemetry =
        SurfaceTelemetry(
            40.0,
            30.0,
            20.0,

            55.0,
            50.0,
            48.0,

            80.0,
            15.0,

            40.0,

            true,
        )

    workload =
        Workload(
            "Test AI",
            AI,

            0.20,
            0.30,
            0.80,

            0.50,
            0.50,
        )

    limits =
        SurfaceLimits(
            95.0,
            95.0,
            95.0,

            5.0,

            65.0,

            95.0,
            95.0,
            95.0,
        )

    weights =
        OptimizationWeights(
            1.0,
            0.20,
            1.0,
            0.50,
        )

    thermal =
        ThermalModel(
            22.0,
            0.08,
            0.10,
            0.08,
            0.015,
        )

    optimizer =
        SurfaceOptimizer(
            limits,
            weights,
            thermal,
        )

    result =
        optimize_surface(
            optimizer,
            telemetry,
            workload,
        )

    @test result !== nothing

    @test result.cpu_target >= 0.0
    @test result.cpu_target <= 1.0

    @test result.gpu_target >= 0.0
    @test result.gpu_target <= 1.0

    @test result.npu_target >= 0.0
    @test result.npu_target <= 1.0

    @test result.display_refresh >= 30
    @test result.display_refresh <= 144

end


@testset "Battery" begin

    @test battery_factor(
        100.0,
        true,
    ) == 1.0

    @test battery_factor(
        5.0,
        false,
    ) < 0.5

end
The important part: this becomes a Surface control loop

The Julia engine can eventually run continuously:

                 Surface Sensors
                       │
                       ▼
              ┌─────────────────┐
              │ Windows/C++     │
              │ Telemetry       │
              └────────┬────────┘
                       │
                       ▼
               ┌───────────────┐
               │ JULIA ENGINE  │
               │               │
               │ Optimisation  │
               │ Thermal model │
               │ Battery model │
               │ Workload      │
               │ prediction    │
               └───────┬───────┘
                       │
                       ▼
                OPTIMAL POLICY
                       │
       ┌───────────────┼────────────────┐
       ▼               ▼                ▼
      CPU              GPU              NPU
       │               │                │
       └───────────────┼────────────────┘
                       │
                       ▼
                    DISPLAY
                    
                    
                    
                    
                    
                    
                    
                    
                    Project
SurfaceControlCenter/
├── SurfaceControlCenter.csproj
├── App.xaml
├── App.xaml.cs
├── MainWindow.xaml
├── MainWindow.xaml.cs
│
├── Models/
│   ├── SurfaceDevice.cs
│   ├── SurfaceTelemetry.cs
│   ├── SurfacePolicy.cs
│   └── SurfaceWorkload.cs
│
├── ViewModels/
│   ├── MainViewModel.cs
│   ├── PerformanceViewModel.cs
│   └── DeviceViewModel.cs
│
├── Services/
│   ├── ISurfaceTelemetryService.cs
│   ├── SurfaceTelemetryService.cs
│   ├── ISurfacePolicyService.cs
│   └── SurfacePolicyService.cs
│
└── Helpers/
    └── ObservableObject.cs
1. SurfaceControlCenter.csproj
<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>WinExe</OutputType>

    <TargetFramework>net8.0-windows10.0.19041.0</TargetFramework>

    <TargetPlatformMinVersion>
      10.0.17763.0
    </TargetPlatformMinVersion>

    <RootNamespace>
      SurfaceControlCenter
    </RootNamespace>

    <ApplicationManifest>
      app.manifest
    </ApplicationManifest>

    <Platforms>x86;x64;ARM64</Platforms>

    <RuntimeIdentifiers>
      win-x64;win-arm64
    </RuntimeIdentifiers>

    <UseWinUI>true</UseWinUI>
    <EnableMsixTooling>true</EnableMsixTooling>

    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>

  <ItemGroup>

    <PackageReference
      Include="Microsoft.WindowsAppSDK"
      Version="1.6.240829007" />

    <PackageReference
      Include="CommunityToolkit.Mvvm"
      Version="8.3.2" />

  </ItemGroup>

</Project>

For a real project, pin the Windows App SDK version you have validated rather than blindly using a newer version.

2. App.xaml
<Application
    x:Class="SurfaceControlCenter.App"
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">

    <Application.Resources>

        <XamlControlsResources
            xmlns="using:Microsoft.UI.Xaml.Controls" />

    </Application.Resources>

</Application>
3. App.xaml.cs
using Microsoft.UI.Xaml;

namespace SurfaceControlCenter;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(
        LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
4. Device model

Models/SurfaceDevice.cs

namespace SurfaceControlCenter.Models;

public sealed class SurfaceDevice
{
    public string Model { get; init; } =
        "Microsoft Surface";

    public string Processor { get; init; } =
        "Unknown";

    public string OperatingSystem { get; init; } =
        "Windows";

    public bool HasNpu { get; init; }

    public bool HasPen { get; init; }

    public bool HasTouch { get; init; }

    public int DisplayRefreshRate { get; init; }

    public string Architecture { get; init; } =
        "Unknown";
}
5. Telemetry model

Models/SurfaceTelemetry.cs

namespace SurfaceControlCenter.Models;

public sealed class SurfaceTelemetry
{
    public double CpuUsage { get; init; }

    public double GpuUsage { get; init; }

    public double NpuUsage { get; init; }

    public double CpuTemperature { get; init; }

    public double GpuTemperature { get; init; }

    public double NpuTemperature { get; init; }

    public double BatteryPercent { get; init; }

    public double BatteryPowerWatts { get; init; }

    public double MemoryUsage { get; init; }

    public bool OnAcPower { get; init; }

    public int DisplayRefreshRate { get; init; }
}
6. Policy model

Models/SurfacePolicy.cs

namespace SurfaceControlCenter.Models;

public sealed class SurfacePolicy
{
    public string PerformanceMode { get; init; } =
        "Balanced";

    public double CpuLimit { get; init; }

    public double GpuLimit { get; init; }

    public double NpuLimit { get; init; }

    public int DisplayRefreshRate { get; init; }

    public double PowerLimitWatts { get; init; }

    public bool BatterySaving { get; init; }

    public bool ThermalProtection { get; init; }
}
7. Workload model

Models/SurfaceWorkload.cs

namespace SurfaceControlCenter.Models;

public sealed class SurfaceWorkload
{
    public string Name { get; init; } = "";

    public double CpuDemand { get; init; }

    public double GpuDemand { get; init; }

    public double NpuDemand { get; init; }

    public bool LatencySensitive { get; init; }
}
8. Observable base

Helpers/ObservableObject.cs

using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace SurfaceControlCenter.Helpers;

public abstract class ObservableObject :
    INotifyPropertyChanged
{
    public event PropertyChangedEventHandler?
        PropertyChanged;

    protected bool SetProperty<T>(
        ref T field,
        T value,
        [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(
                field,
                value))
        {
            return false;
        }

        field = value;

        PropertyChanged?.Invoke(
            this,
            new PropertyChangedEventArgs(propertyName));

        return true;
    }

    protected void OnPropertyChanged(
        [CallerMemberName] string? propertyName = null)
    {
        PropertyChanged?.Invoke(
            this,
            new PropertyChangedEventArgs(propertyName));
    }
}
9. Telemetry service

Services/ISurfaceTelemetryService.cs

using SurfaceControlCenter.Models;

namespace SurfaceControlCenter.Services;

public interface ISurfaceTelemetryService
{
    Task<SurfaceTelemetry> GetTelemetryAsync(
        CancellationToken cancellationToken = default);

    Task<SurfaceDevice> GetDeviceAsync(
        CancellationToken cancellationToken = default);
}
10. Telemetry implementation

Services/SurfaceTelemetryService.cs

using System.Diagnostics;
using System.Runtime.InteropServices;
using SurfaceControlCenter.Models;

namespace SurfaceControlCenter.Services;

public sealed class SurfaceTelemetryService :
    ISurfaceTelemetryService
{
    private readonly Process _process =
        Process.GetCurrentProcess();

    private double _lastCpuTime;

    private DateTime _lastSampleTime =
        DateTime.UtcNow;

    public Task<SurfaceTelemetry> GetTelemetryAsync(
        CancellationToken cancellationToken = default)
    {
        var now = DateTime.UtcNow;

        var processCpu =
            _process.TotalProcessorTime.TotalSeconds;

        var elapsed =
            (now - _lastSampleTime).TotalSeconds;

        double cpuUsage = 0;

        if (elapsed > 0)
        {
            var processors =
                Environment.ProcessorCount;

            cpuUsage =
                (
                    (processCpu - _lastCpuTime)
                    /
                    elapsed
                    /
                    processors
                ) * 100.0;
        }

        _lastCpuTime = processCpu;
        _lastSampleTime = now;

        var power =
            GetSystemPowerStatus();

        return Task.FromResult(
            new SurfaceTelemetry
            {
                CpuUsage =
                    Math.Clamp(cpuUsage, 0, 100),

                GpuUsage = 0,

                NpuUsage = 0,

                CpuTemperature = 0,

                GpuTemperature = 0,

                NpuTemperature = 0,

                BatteryPercent =
                    power.BatteryPercent,

                BatteryPowerWatts = 0,

                MemoryUsage = 0,

                OnAcPower =
                    power.OnAcPower,

                DisplayRefreshRate = 60
            });
    }

    public Task<SurfaceDevice> GetDeviceAsync(
        CancellationToken cancellationToken = default)
    {
        var architecture =
            RuntimeInformation.OSArchitecture
                .ToString();

        return Task.FromResult(
            new SurfaceDevice
            {
                Model =
                    "Microsoft Surface",

                Processor =
                    Environment.GetEnvironmentVariable(
                        "PROCESSOR_IDENTIFIER")
                    ?? "Unknown",

                OperatingSystem =
                    Environment.OSVersion
                        .VersionString,

                Architecture =
                    architecture,

                HasNpu = false,
                HasPen = true,
                HasTouch = true,

                DisplayRefreshRate = 60
            });
    }

    private static (
        double BatteryPercent,
        bool OnAcPower
    ) GetSystemPowerStatus()
    {
        if (!NativeMethods.GetSystemPowerStatus(
                out var status))
        {
            return (0, true);
        }

        var percent =
            status.BatteryLifePercent == 255
                ? 0
                : status.BatteryLifePercent;

        var ac =
            status.ACLineStatus == 1;

        return (percent, ac);
    }

    private static class NativeMethods
    {
        [DllImport(
            "kernel32.dll",
            SetLastError = true)]
        public static extern bool
            GetSystemPowerStatus(
                out SYSTEM_POWER_STATUS status);

        [StructLayout(
            LayoutKind.Sequential)]
        public struct SYSTEM_POWER_STATUS
        {
            public byte ACLineStatus;

            public byte BatteryFlag;

            public byte BatteryLifePercent;

            public byte Reserved;

            public int BatteryLifeTime;

            public int BatteryFullLifeTime;
        }
    }
}

This deliberately doesn't invent GPU/NPU temperatures. Those should eventually come from the native telemetry layer developed in the previous C++ projects.

11. Policy service

Services/ISurfacePolicyService.cs

using SurfaceControlCenter.Models;

namespace SurfaceControlCenter.Services;

public interface ISurfacePolicyService
{
    Task<SurfacePolicy> CalculatePolicyAsync(
        SurfaceTelemetry telemetry,
        CancellationToken cancellationToken = default);
}
12. Policy implementation

Services/SurfacePolicyService.cs

using SurfaceControlCenter.Models;

namespace SurfaceControlCenter.Services;

public sealed class SurfacePolicyService :
    ISurfacePolicyService
{
    public Task<SurfacePolicy> CalculatePolicyAsync(
        SurfaceTelemetry telemetry,
        CancellationToken cancellationToken = default)
    {
        var mode = "Balanced";

        var cpu = 1.0;
        var gpu = 1.0;
        var npu = 1.0;

        var refresh = 60;
        var power = 65.0;

        var batterySaving = false;
        var thermalProtection = false;

        /*
         * Battery optimisation.
         */

        if (!telemetry.OnAcPower)
        {
            if (telemetry.BatteryPercent < 20)
            {
                mode = "Battery Saver";

                cpu = 0.55;
                gpu = 0.45;
                npu = 0.50;

                refresh = 60;

                power = 30;

                batterySaving = true;
            }
            else if (telemetry.BatteryPercent < 40)
            {
                cpu = 0.75;
                gpu = 0.70;
                npu = 0.70;

                power = 45;
            }
        }

        /*
         * Thermal protection.
         */

        if (telemetry.CpuTemperature >= 90 ||
            telemetry.GpuTemperature >= 90 ||
            telemetry.NpuTemperature >= 90)
        {
            mode = "Thermal Protection";

            cpu = Math.Min(cpu, 0.65);
            gpu = Math.Min(gpu, 0.55);
            npu = Math.Min(npu, 0.60);

            refresh = 60;

            power = Math.Min(
                power,
                35
            );

            thermalProtection = true;
        }

        /*
         * High-performance AC operation.
         */

        if (telemetry.OnAcPower &&
            telemetry.BatteryPercent > 30 &&
            !thermalProtection)
        {
            mode = "Performance";

            cpu = 1.0;
            gpu = 1.0;
            npu = 1.0;

            power = 65;
        }

        return Task.FromResult(
            new SurfacePolicy
            {
                PerformanceMode = mode,

                CpuLimit = cpu,
                GpuLimit = gpu,
                NpuLimit = npu,

                DisplayRefreshRate = refresh,

                PowerLimitWatts = power,

                BatterySaving = batterySaving,

                ThermalProtection =
                    thermalProtection
            });
    }
}
13. Performance ViewModel

ViewModels/PerformanceViewModel.cs

using SurfaceControlCenter.Helpers;
using SurfaceControlCenter.Models;
using SurfaceControlCenter.Services;

namespace SurfaceControlCenter.ViewModels;

public sealed class PerformanceViewModel :
    ObservableObject
{
    private readonly ISurfaceTelemetryService
        _telemetryService;

    private readonly ISurfacePolicyService
        _policyService;

    private SurfaceTelemetry? _telemetry;

    private SurfacePolicy? _policy;

    public double CpuUsage =>
        _telemetry?.CpuUsage ?? 0;

    public double GpuUsage =>
        _telemetry?.GpuUsage ?? 0;

    public double NpuUsage =>
        _telemetry?.NpuUsage ?? 0;

    public double Battery =>
        _telemetry?.BatteryPercent ?? 0;

    public double CpuTemperature =>
        _telemetry?.CpuTemperature ?? 0;

    public string PerformanceMode =>
        _policy?.PerformanceMode ?? "Starting";

    public int RefreshRate =>
        _policy?.DisplayRefreshRate ?? 60;

    public double PowerLimit =>
        _policy?.PowerLimitWatts ?? 0;

    public PerformanceViewModel(
        ISurfaceTelemetryService telemetryService,
        ISurfacePolicyService policyService)
    {
        _telemetryService =
            telemetryService;

        _policyService =
            policyService;
    }

    public async Task RefreshAsync()
    {
        _telemetry =
            await _telemetryService
                .GetTelemetryAsync();

        _policy =
            await _policyService
                .CalculatePolicyAsync(
                    _telemetry);

        OnPropertyChanged(
            nameof(CpuUsage));

        OnPropertyChanged(
            nameof(GpuUsage));

        OnPropertyChanged(
            nameof(NpuUsage));

        OnPropertyChanged(
            nameof(Battery));

        OnPropertyChanged(
            nameof(CpuTemperature));

        OnPropertyChanged(
            nameof(PerformanceMode));

        OnPropertyChanged(
            nameof(RefreshRate));

        OnPropertyChanged(
            nameof(PowerLimit));
    }
}
14. Main ViewModel

ViewModels/MainViewModel.cs

using System.Windows.Input;
using SurfaceControlCenter.Helpers;
using SurfaceControlCenter.Models;
using SurfaceControlCenter.Services;

namespace SurfaceControlCenter.ViewModels;

public sealed class MainViewModel :
    ObservableObject
{
    private readonly ISurfaceTelemetryService
        _telemetry;

    private readonly ISurfacePolicyService
        _policy;

    private SurfaceDevice? _device;

    public PerformanceViewModel Performance
    {
        get;
    }

    public string DeviceName =>
        _device?.Model ??
        "Surface";

    public string Processor =>
        _device?.Processor ??
        "Detecting processor...";

    public string Architecture =>
        _device?.Architecture ??
        "Unknown";

    public bool HasTouch =>
        _device?.HasTouch ?? false;

    public bool HasPen =>
        _device?.HasPen ?? false;

    public bool HasNpu =>
        _device?.HasNpu ?? false;

    public ICommand RefreshCommand
    {
        get;
    }

    public MainViewModel()
    {
        _telemetry =
            new SurfaceTelemetryService();

        _policy =
            new SurfacePolicyService();

        Performance =
            new PerformanceViewModel(
                _telemetry,
                _policy);

        RefreshCommand =
            new AsyncCommand(
                RefreshAsync);
    }

    public async Task InitializeAsync()
    {
        _device =
            await _telemetry
                .GetDeviceAsync();

        OnPropertyChanged(
            nameof(DeviceName));

        OnPropertyChanged(
            nameof(Processor));

        OnPropertyChanged(
            nameof(Architecture));

        OnPropertyChanged(
            nameof(HasTouch));

        OnPropertyChanged(
            nameof(HasPen));

        OnPropertyChanged(
            nameof(HasNpu));

        await Performance
            .RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        await Performance
            .RefreshAsync();
    }
}


public sealed class AsyncCommand :
    ICommand
{
    private readonly Func<Task> _execute;

    private bool _running;

    public event EventHandler? CanExecuteChanged;

    public AsyncCommand(
        Func<Task> execute)
    {
        _execute = execute;
    }

    public bool CanExecute(object? parameter)
    {
        return !_running;
    }

    public async void Execute(
        object? parameter)
    {
        if (_running)
            return;

        try
        {
            _running = true;

            CanExecuteChanged?
                .Invoke(this, EventArgs.Empty);

            await _execute();
        }
        finally
        {
            _running = false;

            CanExecuteChanged?
                .Invoke(this, EventArgs.Empty);
        }
    }
}
15. Main window

MainWindow.xaml

<Window
    x:Class="SurfaceControlCenter.MainWindow"
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    xmlns:d="http://schemas.microsoft.com/expression/blend/2008"
    xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"
    mc:Ignorable="d">

    <Grid>

        <Grid.RowDefinitions>
            <RowDefinition Height="72"/>
            <RowDefinition Height="*"/>
        </Grid.RowDefinitions>


        <!-- Header -->

        <Grid
            Grid.Row="0"
            Padding="28,0">

            <Grid.ColumnDefinitions>
                <ColumnDefinition/>
                <ColumnDefinition Width="Auto"/>
            </Grid.ColumnDefinitions>

            <StackPanel
                VerticalAlignment="Center">

                <TextBlock
                    Text="SURFACE CONTROL CENTER"
                    FontSize="22"
                    FontWeight="SemiBold"/>

                <TextBlock
                    Text="Windows / Surface Intelligence"
                    Opacity="0.65"
                    FontSize="12"/>

            </StackPanel>

            <Button
                Grid.Column="1"
                Content="Refresh"
                Command="{Binding RefreshCommand}"
                VerticalAlignment="Center"/>

        </Grid>


        <!-- Main content -->

        <ScrollViewer
            Grid.Row="1">

            <Grid
                Padding="28"
                RowSpacing="18">

                <Grid.RowDefinitions>
                    <RowDefinition Height="Auto"/>
                    <RowDefinition Height="Auto"/>
                    <RowDefinition Height="Auto"/>
                </Grid.RowDefinitions>


                <!-- Device -->

                <Border
                    Grid.Row="0"
                    Padding="24"
                    CornerRadius="14"
                    Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">

                    <Grid>

                        <Grid.ColumnDefinitions>
                            <ColumnDefinition/>
                            <ColumnDefinition Width="Auto"/>
                        </Grid.ColumnDefinitions>

                        <StackPanel>

                            <TextBlock
                                Text="{Binding DeviceName}"
                                FontSize="28"
                                FontWeight="SemiBold"/>

                            <TextBlock
                                Text="{Binding Processor}"
                                Margin="0,6,0,0"/>

                            <TextBlock
                                Text="{Binding Architecture}"
                                Opacity="0.6"
                                Margin="0,3,0,0"/>

                        </StackPanel>

                        <StackPanel
                            Grid.Column="1"
                            HorizontalAlignment="Right">

                            <TextBlock
                                Text="FEATURES"
                                FontSize="11"
                                Opacity="0.6"/>

                            <TextBlock
                                Text="{Binding HasTouch}"
                                Margin="0,5,0,0"/>

                            <TextBlock
                                Text="{Binding HasPen}"
                                Margin="0,5,0,0"/>

                            <TextBlock
                                Text="{Binding HasNpu}"
                                Margin="0,5,0,0"/>

                        </StackPanel>

                    </Grid>

                </Border>


                <!-- Performance -->

                <Grid
                    Grid.Row="1"
                    ColumnSpacing="16">

                    <Grid.ColumnDefinitions>
                        <ColumnDefinition/>
                        <ColumnDefinition/>
                        <ColumnDefinition/>
                    </Grid.ColumnDefinitions>


                    <!-- CPU -->

                    <Border
                        Grid.Column="0"
                        Padding="20"
                        CornerRadius="14"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">

                        <StackPanel>

                            <TextBlock
                                Text="CPU"
                                FontSize="14"/>

                            <TextBlock
                                Text="{Binding Performance.CpuUsage,
                                    StringFormat='{}{0:F0}%'}"
                                FontSize="36"
                                FontWeight="SemiBold"
                                Margin="0,12,0,4"/>

                            <ProgressBar
                                Value="{Binding Performance.CpuUsage}"
                                Maximum="100"/>

                        </StackPanel>

                    </Border>


                    <!-- GPU -->

                    <Border
                        Grid.Column="1"
                        Padding="20"
                        CornerRadius="14"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">

                        <StackPanel>

                            <TextBlock
                                Text="GPU"
                                FontSize="14"/>

                            <TextBlock
                                Text="{Binding Performance.GpuUsage,
                                    StringFormat='{}{0:F0}%'}"
                                FontSize="36"
                                FontWeight="SemiBold"
                                Margin="0,12,0,4"/>

                            <ProgressBar
                                Value="{Binding Performance.GpuUsage}"
                                Maximum="100"/>

                        </StackPanel>

                    </Border>


                    <!-- NPU -->

                    <Border
                        Grid.Column="2"
                        Padding="20"
                        CornerRadius="14"
                        Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">

                        <StackPanel>

                            <TextBlock
                                Text="NPU"
                                FontSize="14"/>

                            <TextBlock
                                Text="{Binding Performance.NpuUsage,
                                    StringFormat='{}{0:F0}%'}"
                                FontSize="36"
                                FontWeight="SemiBold"
                                Margin="0,12,0,4"/>

                            <ProgressBar
                                Value="{Binding Performance.NpuUsage}"
                                Maximum="100"/>

                        </StackPanel>

                    </Border>

                </Grid>


                <!-- Policy -->

                <Border
                    Grid.Row="2"
                    Padding="24"
                    CornerRadius="14"
                    Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">

                    <Grid>

                        <Grid.RowDefinitions>
                            <RowDefinition/>
                            <RowDefinition/>
                            <RowDefinition/>
                        </Grid.RowDefinitions>

                        <TextBlock
                            Grid.Row="0"
                            Text="ACTIVE SYSTEM POLICY"
                            FontSize="12"
                            Opacity="0.65"/>

                        <TextBlock
                            Grid.Row="1"
                            Text="{Binding Performance.PerformanceMode}"
                            FontSize="28"
                            FontWeight="SemiBold"
                            Margin="0,8,0,14"/>

                        <Grid
                            Grid.Row="2">

                            <Grid.ColumnDefinitions>
                                <ColumnDefinition/>
                                <ColumnDefinition/>
                                <ColumnDefinition/>
                            </Grid.ColumnDefinitions>

                            <StackPanel>
                                <TextBlock
                                    Text="Battery"
                                    Opacity="0.6"/>

                                <TextBlock
                                    Text="{Binding Performance.Battery,
                                        StringFormat='{}{0:F0}%'}"
                                    FontSize="20"/>
                            </StackPanel>

                            <StackPanel
                                Grid.Column="1">

                                <TextBlock
                                    Text="Refresh"
                                    Opacity="0.6"/>

                                <TextBlock
                                    Text="{Binding Performance.RefreshRate,
                                        StringFormat='{}{0} Hz'}"
                                    FontSize="20"/>

                            </StackPanel>

                            <StackPanel
                                Grid.Column="2">

                                <TextBlock
                                    Text="Power"
                                    Opacity="0.6"/>

                                <TextBlock
                                    Text="{Binding Performance.PowerLimit,
                                        StringFormat='{}{0:F0} W'}"
                                    FontSize="20"/>

                            </StackPanel>

                        </Grid>

                    </Grid>

                </Border>

            </Grid>

        </ScrollViewer>

    </Grid>

</Window>
16. MainWindow.xaml.cs
using Microsoft.UI.Xaml;
using SurfaceControlCenter.ViewModels;

namespace SurfaceControlCenter;

public sealed partial class MainWindow : Window
{
    public MainViewModel ViewModel
    {
        get;
    }

    public MainWindow()
    {
        InitializeComponent();

        ViewModel =
            new MainViewModel();

        Content =
            new MainWindowContent(
                ViewModel);
    }
}

Because WinUI's generated XAML root is easier to maintain as the application grows, I'd actually use a Page for the content.

MainWindowContent.xaml:

<Page
    x:Class="SurfaceControlCenter.MainWindowContent"
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">

    <!--
        Move the Grid from MainWindow.xaml here.
        DataContext is assigned by MainWindowContent.xaml.cs.
    -->

</Page>

However, for a production implementation, I'd simplify this further and make MainWindow host a dedicated NavigationView and several pages.

17. The more ambitious Surface UI architecture

This is where the previous projects begin fitting together:

                    SURFACE CONTROL CENTER
                             │
                 ┌───────────┴───────────┐
                 │       C# / WinUI      │
                 │                       │
                 │  Dashboard            │
                 │  Performance          │
                 │  Battery              │
                 │  Thermal              │
                 │  Pen                  │
                 │  Touch                │
                 │  Camera               │
                 │  AI                   │
                 │  Security             │
                 └───────────┬───────────┘
                             │
                 Native interop / IPC
                             │
       ┌─────────────────────┼─────────────────────┐
       ▼                     ▼                     ▼
   C++ engines          Rust services         Julia engine
       │                     │                     │
       ▼                     ▼                     ▼
  Hardware             Security / IPC       Optimisation
  graphics             identity             prediction
  input                crypto               scheduling
  camera               integrity             thermal model
       │                     │                     │
       └─────────────────────┼─────────────────────┘
                             ▼
                       Windows / drivers
                             │
                             ▼
                        Surface hardware
What C# is particularly good for here
Surface function	Language
WinUI interface	C#
Settings / control panels	C#
Device dashboard	C#
Accessibility UI	C#
Notifications	C#
Windows application services	C# / .NET
Native hardware layer	C++
High-performance input	C++
Security services	Rust
Mathematical optimisation	Julia
AI orchestration	C++ + Python
Firmware	C
GPU shaders	HLSL

The key architectural principle is not to make C# do everything. C# should be the polished human-facing layer, while the C++/Rust/Julia components we've been building r









15 — Rust + C++ Surface Pen Intelligence

This takes the #3 C++ Pen Engine and adds a Rust intelligence layer above it.

The split is deliberate:

C++ → Windows pointer input, high-frequency event handling, filtering, device integration.
Rust → gesture intelligence, trajectory analysis, stroke classification, confidence scoring, bounded prediction.
C# / WinUI → UI and presentation.
Julia → could later optimise the prediction/classification parameters.
Surface Pen
     │
     ▼
Windows Pointer API
     │
     ▼
C++ Pen Input Engine
     │
     ├── pressure
     ├── tilt
     ├── position
     ├── timestamp
     └── filtering
     │
     ▼
Rust Pen Intelligence
     │
     ├── trajectory
     ├── velocity
     ├── acceleration
     ├── stroke classification
     ├── gesture recognition
     ├── prediction
     └── confidence
     │
     ▼
C# / WinUI
Project
SurfacePenIntelligence/
│
├── Cargo.toml
│
├── rust/
│   ├── src/
│   │   ├── lib.rs
│   │   ├── types.rs
│   │   ├── trajectory.rs
│   │   ├── velocity.rs
│   │   ├── classifier.rs
│   │   ├── predictor.rs
│   │   ├── gesture.rs
│   │   ├── intelligence.rs
│   │   └── ffi.rs
│   └── tests/
│       └── intelligence_tests.rs
│
└── cpp/
    ├── include/
    │   ├── PenPoint.hpp
    │   ├── PenIntelligenceBridge.hpp
    │   └── SurfacePenEngine.hpp
    │
    └── src/
        ├── PenIntelligenceBridge.cpp
        ├── SurfacePenEngine.cpp
        └── main.cpp
1. Rust Cargo.toml
[package]
name = "surface_pen_intelligence"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["rlib", "cdylib"]

[dependencies]
serde = { version = "1", features = ["derive"] }
thiserror = "2"
2. Rust types

rust/src/types.rs

use serde::{Deserialize, Serialize};

#[derive(
    Debug,
    Clone,
    Copy,
    Serialize,
    Deserialize
)]
pub struct PenPoint {
    pub x: f32,
    pub y: f32,
    pub pressure: f32,
    pub tilt_x: f32,
    pub tilt_y: f32,
    pub rotation: f32,
    pub timestamp_ms: f64,
}

#[derive(
    Debug,
    Clone,
    Copy
)]
pub struct Velocity {
    pub x: f32,
    pub y: f32,
    pub magnitude: f32,
}

#[derive(
    Debug,
    Clone,
    Copy
)]
pub struct Acceleration {
    pub x: f32,
    pub y: f32,
    pub magnitude: f32,
}

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq
)]
pub enum StrokeType {
    Unknown,
    Writing,
    Drawing,
    Signature,
    Diagram,
    FastGesture,
}

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq
)]
pub enum GestureType {
    None,
    Tap,
    DoubleTap,
    LongPress,
    Swipe,
    Flick,
    Circle,
    HorizontalLine,
    VerticalLine,
}

#[derive(Debug, Clone)]
pub struct Prediction {
    pub x: f32,
    pub y: f32,
    pub confidence: f32,
}

#[derive(Debug, Clone)]
pub struct IntelligenceResult {
    pub stroke_type: StrokeType,
    pub gesture: GestureType,

    pub velocity: Velocity,
    pub acceleration: Acceleration,

    pub prediction: Prediction,

    pub confidence: f32,
}
3. Velocity engine

rust/src/velocity.rs

use crate::types::*;

pub fn calculate_velocity(
    previous: &PenPoint,
    current: &PenPoint,
) -> Velocity {
    let dt =
        (current.timestamp_ms -
         previous.timestamp_ms)
        / 1000.0;

    if dt <= 0.000001 {
        return Velocity {
            x: 0.0,
            y: 0.0,
            magnitude: 0.0,
        };
    }

    let vx =
        (current.x - previous.x)
        / dt as f32;

    let vy =
        (current.y - previous.y)
        / dt as f32;

    let magnitude =
        (vx * vx + vy * vy).sqrt();

    Velocity {
        x: vx,
        y: vy,
        magnitude,
    }
}


pub fn calculate_acceleration(
    previous: Velocity,
    current: Velocity,
    dt_seconds: f32,
) -> Acceleration {
    if dt_seconds <= 0.000001 {
        return Acceleration {
            x: 0.0,
            y: 0.0,
            magnitude: 0.0,
        };
    }

    let ax =
        (current.x - previous.x)
        / dt_seconds;

    let ay =
        (current.y - previous.y)
        / dt_seconds;

    Acceleration {
        x: ax,
        y: ay,
        magnitude:
            (ax * ax + ay * ay).sqrt(),
    }
}
4. Trajectory engine

rust/src/trajectory.rs

use crate::types::*;

pub struct Trajectory {
    points: Vec<PenPoint>,
    maximum_points: usize,
}

impl Trajectory {
    pub fn new(maximum_points: usize) -> Self {
        Self {
            points: Vec::with_capacity(
                maximum_points
            ),
            maximum_points,
        }
    }

    pub fn push(
        &mut self,
        point: PenPoint,
    ) {
        self.points.push(point);

        if self.points.len()
            > self.maximum_points
        {
            self.points.remove(0);
        }
    }

    pub fn clear(&mut self) {
        self.points.clear();
    }

    pub fn len(&self) -> usize {
        self.points.len()
    }

    pub fn points(&self) -> &[PenPoint] {
        &self.points
    }

    pub fn total_distance(&self) -> f32 {
        let mut total = 0.0;

        for pair in self.points.windows(2) {
            let dx =
                pair[1].x - pair[0].x;

            let dy =
                pair[1].y - pair[0].y;

            total +=
                (dx * dx + dy * dy).sqrt();
        }

        total
    }

    pub fn displacement(&self) -> f32 {
        if self.points.len() < 2 {
            return 0.0;
        }

        let first =
            self.points.first().unwrap();

        let last =
            self.points.last().unwrap();

        let dx = last.x - first.x;
        let dy = last.y - first.y;

        (dx * dx + dy * dy).sqrt()
    }

    pub fn straightness(&self) -> f32 {
        let distance =
            self.total_distance();

        if distance <= 0.0001 {
            return 1.0;
        }

        (
            self.displacement()
            / distance
        ).clamp(0.0, 1.0)
    }
}
5. Stroke classifier

This is deliberately heuristic rather than pretending to be an ML model.

rust/src/classifier.rs

use crate::trajectory::Trajectory;
use crate::types::*;

pub fn classify_stroke(
    trajectory: &Trajectory,
    velocity: Velocity,
) -> (StrokeType, f32) {
    if trajectory.len() < 3 {
        return (
            StrokeType::Unknown,
            0.0
        );
    }

    let distance =
        trajectory.total_distance();

    let straightness =
        trajectory.straightness();

    /*
     * Very fast, relatively straight
     * movement.
     */
    if velocity.magnitude > 1800.0 &&
       straightness > 0.80
    {
        return (
            StrokeType::FastGesture,
            0.90
        );
    }

    /*
     * Deliberate, relatively straight
     * strokes are often diagrams,
     * underlining or annotation.
     */
    if straightness > 0.88 &&
       distance > 40.0
    {
        return (
            StrokeType::Diagram,
            0.72
        );
    }

    /*
     * Default pen movement.
     */
    (
        StrokeType::Writing,
        0.60
    )
}
6. Gesture recognition

rust/src/gesture.rs

use crate::trajectory::Trajectory;
use crate::types::*;

pub fn detect_gesture(
    trajectory: &Trajectory,
    velocity: Velocity,
) -> GestureType {
    if trajectory.len() < 2 {
        return GestureType::None;
    }

    let points =
        trajectory.points();

    let first =
        points.first().unwrap();

    let last =
        points.last().unwrap();

    let dx =
        last.x - first.x;

    let dy =
        last.y - first.y;

    let distance =
        (dx * dx + dy * dy).sqrt();

    let duration =
        last.timestamp_ms -
        first.timestamp_ms;

    /*
     * Tap.
     */
    if distance < 10.0 &&
       duration < 250.0
    {
        return GestureType::Tap;
    }

    /*
     * Long press.
     */
    if distance < 15.0 &&
       duration > 600.0
    {
        return GestureType::LongPress;
    }

    /*
     * Fast movement.
     */
    if velocity.magnitude > 1800.0 &&
       distance > 100.0
    {
        return GestureType::Flick;
    }

    /*
     * Horizontal gesture.
     */
    if dx.abs() > 100.0 &&
       dy.abs() < 30.0
    {
        return GestureType::HorizontalLine;
    }

    /*
     * Vertical gesture.
     */
    if dy.abs() > 100.0 &&
       dx.abs() < 30.0
    {
        return GestureType::VerticalLine;
    }

    GestureType::None
}
7. Prediction

rust/src/predictor.rs

use crate::types::*;

pub fn predict_position(
    point: &PenPoint,
    velocity: &Velocity,
    prediction_ms: f32,
) -> Prediction {
    let dt =
        prediction_ms / 1000.0;

    /*
     * Bound prediction to prevent
     * runaway extrapolation.
     */
    let dt =
        dt.clamp(0.0, 0.030);

    let x =
        point.x +
        velocity.x * dt;

    let y =
        point.y +
        velocity.y * dt;

    let speed =
        velocity.magnitude;

    let confidence =
        if speed < 100.0 {
            0.95
        } else if speed < 1000.0 {
            0.85
        } else if speed < 2500.0 {
            0.65
        } else {
            0.40
        };

    Prediction {
        x,
        y,
        confidence,
    }
}
8. Intelligence engine

rust/src/intelligence.rs

use crate::{
    classifier::classify_stroke,
    gesture::detect_gesture,
    predictor::predict_position,
    trajectory::Trajectory,
    types::*,
    velocity::{
        calculate_acceleration,
        calculate_velocity,
    },
};

pub struct PenIntelligence {
    trajectory: Trajectory,

    previous_point: Option<PenPoint>,
    previous_velocity: Velocity,

    prediction_ms: f32,
}

impl PenIntelligence {
    pub fn new() -> Self {
        Self {
            trajectory:
                Trajectory::new(128),

            previous_point:
                None,

            previous_velocity:
                Velocity {
                    x: 0.0,
                    y: 0.0,
                    magnitude: 0.0,
                },

            prediction_ms: 8.0,
        }
    }

    pub fn set_prediction_ms(
        &mut self,
        milliseconds: f32,
    ) {
        self.prediction_ms =
            milliseconds.clamp(
                0.0,
                30.0,
            );
    }

    pub fn begin_stroke(&mut self) {
        self.trajectory.clear();

        self.previous_point = None;

        self.previous_velocity =
            Velocity {
                x: 0.0,
                y: 0.0,
                magnitude: 0.0,
            };
    }

    pub fn process(
        &mut self,
        point: PenPoint,
    ) -> IntelligenceResult {
        let velocity =
            match self.previous_point {
                Some(previous) => {
                    calculate_velocity(
                        &previous,
                        &point,
                    )
                }

                None => {
                    Velocity {
                        x: 0.0,
                        y: 0.0,
                        magnitude: 0.0,
                    }
                }
            };

        let acceleration =
            match self.previous_point {
                Some(previous) => {
                    let dt =
                        (
                            point.timestamp_ms -
                            previous.timestamp_ms
                        ) / 1000.0;

                    calculate_acceleration(
                        self.previous_velocity,
                        velocity,
                        dt as f32,
                    )
                }

                None => {
                    Acceleration {
                        x: 0.0,
                        y: 0.0,
                        magnitude: 0.0,
                    }
                }
            };

        self.trajectory.push(point);

        let (
            stroke_type,
            stroke_confidence
        ) =
            classify_stroke(
                &self.trajectory,
                velocity,
            );

        let gesture =
            detect_gesture(
                &self.trajectory,
                velocity,
            );

        let prediction =
            predict_position(
                &point,
                &velocity,
                self.prediction_ms,
            );

        self.previous_point =
            Some(point);

        self.previous_velocity =
            velocity;

        let confidence =
            (
                stroke_confidence * 0.5 +
                prediction.confidence * 0.5
            )
            .clamp(0.0, 1.0);

        IntelligenceResult {
            stroke_type,
            gesture,
            velocity,
            acceleration,
            prediction,
            confidence,
        }
    }
}
9. Rust C ABI

This is the important bridge between the Rust intelligence system and the existing C++ Surface engine.

rust/src/ffi.rs

use std::slice;

use crate::{
    intelligence::PenIntelligence,
    types::PenPoint,
};

#[repr(C)]
pub struct CPenPoint {
    pub x: f32,
    pub y: f32,

    pub pressure: f32,

    pub tilt_x: f32,
    pub tilt_y: f32,

    pub rotation: f32,

    pub timestamp_ms: f64,
}

#[repr(C)]
pub struct CIntelligenceResult {
    pub predicted_x: f32,
    pub predicted_y: f32,

    pub velocity_x: f32,
    pub velocity_y: f32,

    pub velocity_magnitude: f32,

    pub acceleration_magnitude: f32,

    pub confidence: f32,

    pub stroke_type: u32,
    pub gesture_type: u32,
}


#[no_mangle]
pub extern "C" fn pen_intelligence_create()
    -> *mut PenIntelligence
{
    Box::into_raw(
        Box::new(
            PenIntelligence::new()
        )
    )
}


#[no_mangle]
pub unsafe extern "C" fn pen_intelligence_destroy(
    ptr: *mut PenIntelligence,
) {
    if !ptr.is_null() {
        drop(Box::from_raw(ptr));
    }
}


#[no_mangle]
pub unsafe extern "C" fn pen_intelligence_begin_stroke(
    ptr: *mut PenIntelligence,
) {
    if let Some(engine) = ptr.as_mut() {
        engine.begin_stroke();
    }
}


#[no_mangle]
pub unsafe extern "C" fn pen_intelligence_process(
    ptr: *mut PenIntelligence,
    input: *const CPenPoint,
) -> CIntelligenceResult {
    let engine =
        &mut *ptr;

    let p =
        &*input;

    let point =
        PenPoint {
            x: p.x,
            y: p.y,
            pressure: p.pressure,
            tilt_x: p.tilt_x,
            tilt_y: p.tilt_y,
            rotation: p.rotation,
            timestamp_ms: p.timestamp_ms,
        };

    let result =
        engine.process(point);

    CIntelligenceResult {
        predicted_x:
            result.prediction.x,

        predicted_y:
            result.prediction.y,

        velocity_x:
            result.velocity.x,

        velocity_y:
            result.velocity.y,

        velocity_magnitude:
            result.velocity.magnitude,

        acceleration_magnitude:
            result.acceleration.magnitude,

        confidence:
            result.confidence,

        stroke_type:
            result.stroke_type as u32,

        gesture_type:
            result.gesture as u32,
    }
}
10. rust/src/lib.rs
pub mod classifier;
pub mod ffi;
pub mod gesture;
pub mod intelligence;
pub mod predictor;
pub mod trajectory;
pub mod types;
pub mod velocity;

pub use intelligence::PenIntelligence;
pub use types::*;
11. C++ bridge

Now the existing C++ Surface Pen engine can call the Rust library.

cpp/include/PenIntelligenceBridge.hpp

#pragma once

#include <cstdint>
#include <memory>

struct CPenPoint
{
    float x;
    float y;

    float pressure;

    float tiltX;
    float tiltY;

    float rotation;

    double timestampMs;
};

struct CIntelligenceResult
{
    float predictedX;
    float predictedY;

    float velocityX;
    float velocityY;

    float velocityMagnitude;

    float accelerationMagnitude;

    float confidence;

    std::uint32_t strokeType;
    std::uint32_t gestureType;
};

extern "C"
{
    void* pen_intelligence_create();

    void pen_intelligence_destroy(
        void* engine);

    void pen_intelligence_begin_stroke(
        void* engine);

    CIntelligenceResult
    pen_intelligence_process(
        void* engine,
        const CPenPoint* point);
}


class PenIntelligenceBridge
{
public:

    PenIntelligenceBridge();

    ~PenIntelligenceBridge();

    PenIntelligenceBridge(
        const PenIntelligenceBridge&) = delete;

    PenIntelligenceBridge& operator=(
        const PenIntelligenceBridge&) = delete;

    void beginStroke();

    CIntelligenceResult process(
        const CPenPoint& point);

private:

    void* engine_;
};
12. Bridge implementation

cpp/src/PenIntelligenceBridge.cpp

#include "PenIntelligenceBridge.hpp"

#include <stdexcept>

PenIntelligenceBridge::
PenIntelligenceBridge()
    : engine_(
        pen_intelligence_create())
{
    if (engine_ == nullptr)
    {
        throw std::runtime_error(
            "Failed to initialise Rust "
            "pen intelligence engine."
        );
    }
}


PenIntelligenceBridge::
~PenIntelligenceBridge()
{
    if (engine_ != nullptr)
    {
        pen_intelligence_destroy(
            engine_);

        engine_ = nullptr;
    }
}


void PenIntelligenceBridge::
beginStroke()
{
    pen_intelligence_begin_stroke(
        engine_);
}


CIntelligenceResult
PenIntelligenceBridge::
process(
    const CPenPoint& point)
{
    return pen_intelligence_process(
        engine_,
        &point);
}
13. C++ Surface Pen Engine

cpp/include/SurfacePenEngine.hpp

#pragma once

#include "PenIntelligenceBridge.hpp"

#include <functional>
#include <mutex>

struct SurfacePenPoint
{
    float x{};
    float y{};

    float pressure{};

    float tiltX{};
    float tiltY{};

    float rotation{};

    double timestampMs{};

    bool tipDown{};
};


class SurfacePenEngine
{
public:

    using IntelligenceCallback =
        std::function<void(
            const CIntelligenceResult&)>;

    SurfacePenEngine();

    void setCallback(
        IntelligenceCallback callback);

    void beginStroke();

    void processPoint(
        const SurfacePenPoint& point);

private:

    PenIntelligenceBridge
        intelligence_;

    IntelligenceCallback
        callback_;

    std::mutex mutex_;
};
14. C++ implementation

cpp/src/SurfacePenEngine.cpp

#include "SurfacePenEngine.hpp"

SurfacePenEngine::
SurfacePenEngine() = default;


void SurfacePenEngine::
setCallback(
    IntelligenceCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}


void SurfacePenEngine::
beginStroke()
{
    std::lock_guard lock(mutex_);

    intelligence_.beginStroke();
}


void SurfacePenEngine::
processPoint(
    const SurfacePenPoint& point)
{
    CIntelligenceResult result;

    {
        std::lock_guard lock(mutex_);

        CPenPoint input
        {
            point.x,
            point.y,

            point.pressure,

            point.tiltX,
            point.tiltY,

            point.rotation,

            point.timestampMs
        };

        result =
            intelligence_.process(
                input);
    }

    if (callback_)
    {
        callback_(result);
    }
}
15. C++ demo

cpp/src/main.cpp

#include "SurfacePenEngine.hpp"

#include <iostream>


int main()
{
    SurfacePenEngine engine;

    engine.setCallback(
        [](const CIntelligenceResult& result)
        {
            std::cout
                << "Prediction: "
                << result.predictedX
                << ", "
                << result.predictedY
                << "\n";

            std::cout
                << "Velocity: "
                << result.velocityMagnitude
                << " px/s\n";

            std::cout
                << "Confidence: "
                << result.confidence
                << "\n";

            std::cout
                << "Stroke type: "
                << result.strokeType
                << "\n";

            std::cout
                << "Gesture: "
                << result.gestureType
                << "\n\n";
        });


    engine.beginStroke();


    for (int i = 0; i < 100; ++i)
    {
        SurfacePenPoint point;

        point.x =
            100.0f +
            static_cast<float>(i) * 4.0f;

        point.y =
            200.0f +
            static_cast<float>(i) * 1.5f;

        point.pressure = 0.5f;

        point.tiltX = 0.0f;
        point.tiltY = 0.0f;

        point.rotation = 0.0f;

        point.timestampMs =
            static_cast<double>(i) * 8.0;

        point.tipDown = true;

        engine.processPoint(point);
    }

    return 0;
}
16. Rust tests

rust/tests/intelligence_tests.rs

use surface_pen_intelligence::{
    PenIntelligence,
    PenPoint,
};


#[test]
fn trajectory_produces_prediction()
{
    let mut engine =
        PenIntelligence::new();

    engine.begin_stroke();

    let first =
        PenPoint {
            x: 100.0,
            y: 100.0,
            pressure: 0.5,
            tilt_x: 0.0,
            tilt_y: 0.0,
            rotation: 0.0,
            timestamp_ms: 0.0,
        };

    let second =
        PenPoint {
            x: 110.0,
            y: 100.0,
            pressure: 0.5,
            tilt_x: 0.0,
            tilt_y: 0.0,
            rotation: 0.0,
            timestamp_ms: 10.0,
        };

    engine.process(first);

    let result =
        engine.process(second);

    assert!(
        result.prediction.x > 110.0
    );

    assert!(
        result.prediction.confidence
        > 0.0
    );
}


#[test]
fn fast_stroke_has_velocity()
{
    let mut engine =
        PenIntelligence::new();

    engine.begin_stroke();

    engine.process(
        PenPoint {
            x: 0.0,
            y: 0.0,
            pressure: 0.5,
            tilt_x: 0.0,
            tilt_y: 0.0,
            rotation: 0.0,
            timestamp_ms: 0.0,
        }
    );

    let result =
        engine.process(
            PenPoint {
                x: 100.0,
                y: 0.0,
                pressure: 0.5,
                tilt_x: 0.0,
                tilt_y: 0.0,
                rotation: 0.0,
                timestamp_ms: 20.0,
            }
        );

    assert!(
        result.velocity.magnitude
        > 1000.0
    );
}
17. Build architecture

The production build becomes:

                 ┌───────────────────────┐
                 │     C# / WinUI 3      │
                 │                       │
                 │ Surface Notes         │
                 │ InkCanvas             │
                 │ Settings              │
                 │ Pen configuration     │
                 └───────────┬───────────┘
                             │
                         C++/WinRT
                             │
                             ▼
                 ┌───────────────────────┐
                 │      C++ ENGINE       │
                 │                       │
                 │ Windows pointer       │
                 │ sampling              │
                 │ filtering             │
                 │ palm rejection        │
                 │ device integration    │
                 └───────────┬───────────┘
                             │
                           C ABI
                             │
                             ▼
                 ┌───────────────────────┐
                 │    RUST INTELLIGENCE  │
                 │                       │
                 │ trajectory            │
                 │ velocity              │
                 │ acceleration          │
                 │ prediction            │
                 │ stroke classification │
                 │ gesture recognition    │
                 │ confidence             │
                 └───────────┬───────────┘
                             │
                             ▼
                       Pen prediction
                       + gesture event
                       
                       
                       
                       
                       
                       
                       
                       
                       Project
SurfaceBatteryModel/
├── Project.toml
├── src/
│   ├── SurfaceBatteryModel.jl
│   ├── types.jl
│   ├── battery.jl
│   ├── power_model.jl
│   ├── thermal_model.jl
│   ├── soc_estimator.jl
│   ├── runtime_predictor.jl
│   ├── charging_model.jl
│   ├── ageing_model.jl
│   ├── optimizer.jl
│   └── api.jl
│
├── examples/
│   └── battery_demo.jl
│
└── test/
    └── runtests.jl
1. Project.toml
name = "SurfaceBatteryModel"
uuid = "4c5a0b3f-6c7e-4b9a-9f01-123456789abc"
authors = ["Surface Systems"]
version = "0.1.0"

[deps]
JSON3 = "0f8b85d8-1f1d-4b7e-9f5a-2a0f7b7c9a1d"

[compat]
julia = "1.10"
JSON3 = "1"
2. Battery types

src/types.jl

@enum BatteryState
    Discharging
    Charging
    FullyCharged
    Unknown
end


struct BatteryTelemetry

    timestamp::Float64

    voltage::Float64

    current::Float64

    temperature::Float64

    reported_soc::Float64

    state::BatteryState

    cpu_power::Float64

    gpu_power::Float64

    npu_power::Float64

    display_power::Float64

    memory_power::Float64

    storage_power::Float64

    network_power::Float64

    other_power::Float64

    on_ac::Bool
end


struct BatteryModelParameters

    nominal_voltage::Float64

    nominal_capacity_wh::Float64

    charge_efficiency::Float64

    discharge_efficiency::Float64

    idle_power_watts::Float64

    thermal_coefficient::Float64

    ageing_factor::Float64
end


struct BatteryStateEstimate

    soc::Float64

    energy_wh::Float64

    voltage::Float64

    current::Float64

    temperature::Float64

    power_watts::Float64

    health::Float64
end


struct RuntimePrediction

    minutes_remaining::Float64

    hours_remaining::Float64

    energy_remaining_wh::Float64

    average_power_watts::Float64

    confidence::Float64
end


struct ChargingPrediction

    minutes_to_full::Float64

    target_soc::Float64

    charging_power_watts::Float64

    confidence::Float64
end
3. Battery electrical model

src/battery.jl

function battery_power(
    voltage::Float64,
    current::Float64,
)
    return abs(
        voltage * current
    )
end


function clamp_soc(
    soc::Float64,
)
    return clamp(
        soc,
        0.0,
        100.0,
    )
end


function energy_from_soc(
    soc::Float64,
    capacity_wh::Float64,
)
    return (
        clamp_soc(soc)
        / 100.0
    ) * capacity_wh
end


function soc_from_energy(
    energy_wh::Float64,
    capacity_wh::Float64,
)
    if capacity_wh <= 0
        return 0.0
    end

    return clamp(
        energy_wh /
        capacity_wh *
        100.0,

        0.0,
        100.0,
    )
end
4. Total Surface power model

src/power_model.jl

function total_system_power(
    telemetry::BatteryTelemetry,
)
    return (
        telemetry.cpu_power +
        telemetry.gpu_power +
        telemetry.npu_power +
        telemetry.display_power +
        telemetry.memory_power +
        telemetry.storage_power +
        telemetry.network_power +
        telemetry.other_power
    )
end


function system_power_breakdown(
    telemetry::BatteryTelemetry,
)
    total =
        total_system_power(
            telemetry
        )

    return (
        cpu =
            telemetry.cpu_power,

        gpu =
            telemetry.gpu_power,

        npu =
            telemetry.npu_power,

        display =
            telemetry.display_power,

        memory =
            telemetry.memory_power,

        storage =
            telemetry.storage_power,

        network =
            telemetry.network_power,

        other =
            telemetry.other_power,

        total =
            total,
    )
end


function weighted_average_power(
    samples::Vector{BatteryTelemetry},
)
    if isempty(samples)
        return 0.0
    end

    total =
        sum(
            total_system_power(s)
            for s in samples
        )

    return total / length(samples)
end
5. Thermal battery model

Battery performance isn't completely independent of temperature.

src/thermal_model.jl

function temperature_efficiency(
    temperature::Float64,
)
    if temperature < 0.0
        return 0.75

    elseif temperature < 10.0
        return 0.90

    elseif temperature <= 35.0
        return 1.00

    elseif temperature <= 45.0
        return 0.95

    elseif temperature <= 50.0
        return 0.80

    else
        return 0.60
    end
end


function thermal_power_penalty(
    temperature::Float64,
)
    if temperature <= 30.0
        return 0.0
    end

    excess =
        temperature - 30.0

    return 0.02 * excess
end


function effective_capacity(
    nominal_capacity_wh::Float64,
    temperature::Float64,
)
    factor =
        temperature_efficiency(
            temperature
        )

    return (
        nominal_capacity_wh *
        factor
    )
end

These values are model parameters, not claims about a particular Surface battery. For real hardware they should be fitted against measured battery data.

6. SOC estimator

This combines the reported operating-system SOC with coulomb-style integration.

src/soc_estimator.jl

mutable struct SOCEstimator

    soc::Float64

    last_timestamp::Float64

    initialized::Bool

    alpha::Float64
end


function SOCEstimator(
    initial_soc::Float64 = 100.0;
    alpha::Float64 = 0.15,
)
    return SOCEstimator(
        clamp_soc(initial_soc),
        0.0,
        false,
        clamp(
            alpha,
            0.0,
            1.0,
        ),
    )
end


function update!(
    estimator::SOCEstimator,
    telemetry::BatteryTelemetry,
    parameters::BatteryModelParameters,
)
    if !estimator.initialized

        estimator.soc =
            clamp_soc(
                telemetry.reported_soc
            )

        estimator.last_timestamp =
            telemetry.timestamp

        estimator.initialized =
            true

        return estimator.soc
    end


    dt =
        telemetry.timestamp -
        estimator.last_timestamp


    if dt <= 0
        return estimator.soc
    end


    /*
     * Positive current is treated
     * as discharge.
     */

    discharge_current =
        max(
            telemetry.current,
            0.0,
        )


    energy_used =
        discharge_current *
        telemetry.voltage *
        dt /
        3600.0


    effective_capacity_wh =
        effective_capacity(
            parameters.nominal_capacity_wh,
            telemetry.temperature,
        )


    soc_delta =
        energy_used /
        effective_capacity_wh *
        100.0


    integrated_soc =
        estimator.soc -
        soc_delta


    /*
     * Blend with the operating system's
     * reported SOC to limit long-term drift.
     */

    reported =
        clamp_soc(
            telemetry.reported_soc
        )


    estimator.soc =
        clamp_soc(
            (
                (1.0 - estimator.alpha)
                *
                integrated_soc
            )
            +
            (
                estimator.alpha
                *
                reported
            )
        )


    estimator.last_timestamp =
        telemetry.timestamp


    return estimator.soc
end
7. Runtime predictor

src/runtime_predictor.jl

function predict_runtime(
    estimate::BatteryStateEstimate,
    average_power_watts::Float64,
)
    if average_power_watts <= 0.01
        return RuntimePrediction(
            Inf,
            Inf,
            estimate.energy_wh,
            average_power_watts,
            0.0,
        )
    end


    usable_energy =
        max(
            0.0,
            estimate.energy_wh,
        )


    hours =
        usable_energy /
        average_power_watts


    minutes =
        hours * 60.0


    /*
     * Simple confidence model.
     *
     * Real implementation should use
     * variance of recent power samples.
     */

    confidence =
        if average_power_watts < 10
            0.70
        elseif average_power_watts < 30
            0.85
        elseif average_power_watts < 60
            0.75
        else
            0.60
        end


    return RuntimePrediction(
        minutes,
        hours,
        usable_energy,
        average_power_watts,
        confidence,
    )
end


function runtime_from_history(
    estimate::BatteryStateEstimate,
    samples::Vector{BatteryTelemetry},
)
    average =
        weighted_average_power(
            samples
        )

    return predict_runtime(
        estimate,
        average,
    )
end
8. Charging model

src/charging_model.jl

function charging_power(
    voltage::Float64,
    current::Float64,
)
    return max(
        0.0,
        abs(voltage * current)
    )
end


function estimate_charge_time(
    current_soc::Float64,
    target_soc::Float64,
    capacity_wh::Float64,
    charging_power_watts::Float64,
    efficiency::Float64,
)
    if charging_power_watts <= 0
        return Inf
    end

    remaining_soc =
        max(
            0.0,
            target_soc -
            current_soc,
        )

    required_energy =
        remaining_soc /
        100.0 *
        capacity_wh

    effective_power =
        charging_power_watts *
        efficiency

    hours =
        required_energy /
        effective_power

    return hours * 60.0
end


function predict_charging(
    estimate::BatteryStateEstimate,
    parameters::BatteryModelParameters,
    charging_power_watts::Float64,
    target_soc::Float64 = 100.0,
)
    minutes =
        estimate_charge_time(
            estimate.soc,
            target_soc,
            parameters.nominal_capacity_wh,
            charging_power_watts,
            parameters.charge_efficiency,
        )

    return ChargingPrediction(
        minutes,
        target_soc,
        charging_power_watts,
        0.70,
    )
end
9. Battery ageing model

src/ageing_model.jl

function ageing_from_cycles(
    cycle_count::Float64,
)
    /*
     * Simplified model.
     *
     * Replace with chemistry-specific
     * empirical data for production.
     */

    degradation =
        0.00008 *
        cycle_count

    return clamp(
        1.0 - degradation,
        0.50,
        1.0,
    )
end


function ageing_from_temperature(
    average_temperature::Float64,
)
    if average_temperature <= 30.0
        return 1.0
    end

    excess =
        average_temperature - 30.0

    degradation =
        excess * 0.002

    return clamp(
        1.0 - degradation,
        0.70,
        1.0,
    )
end


function battery_health(
    cycle_count::Float64,
    average_temperature::Float64,
)
    cycle_health =
        ageing_from_cycles(
            cycle_count
        )

    thermal_health =
        ageing_from_temperature(
            average_temperature
        )

    return (
        cycle_health *
        thermal_health
    )
end
10. Main battery model

src/optimizer.jl

struct BatteryEngine

    parameters::BatteryModelParameters

    estimator::SOCEstimator

    power_history::Vector{Float64}

    maximum_history::Int
end


function BatteryEngine(
    parameters::BatteryModelParameters;
    initial_soc::Float64 = 100.0,
    history_size::Int = 300,
)
    return BatteryEngine(
        parameters,

        SOCEstimator(
            initial_soc
        ),

        Float64[],

        history_size,
    )
end


function update!(
    engine::BatteryEngine,
    telemetry::BatteryTelemetry,
)
    soc =
        update!(
            engine.estimator,
            telemetry,
            engine.parameters,
        )


    power =
        total_system_power(
            telemetry
        )


    push!(
        engine.power_history,
        power,
    )


    if length(
        engine.power_history
    ) > engine.maximum_history

        popfirst!(
            engine.power_history
        )

    end


    effective_capacity_wh =
        effective_capacity(
            engine.parameters
                .nominal_capacity_wh,

            telemetry.temperature,
        )


    energy =
        energy_from_soc(
            soc,
            effective_capacity_wh,
        )


    health =
        battery_health(
            0.0,
            telemetry.temperature,
        )


    estimate =
        BatteryStateEstimate(
            soc,
            energy,
            telemetry.voltage,
            telemetry.current,
            telemetry.temperature,
            power,
            health,
        )


    average_power =
        isempty(
            engine.power_history
        ) ?

        power :

        sum(
            engine.power_history
        )
        /
        length(
            engine.power_history
        )


    runtime =
        predict_runtime(
            estimate,
            average_power,
        )


    return (
        estimate = estimate,
        runtime = runtime,
        average_power = average_power,
    )
end
11. Power-budget optimiser

This is where the battery model becomes an actual control system.

src/optimizer.jl

function optimise_power_budget(
    available_energy_wh::Float64,
    target_runtime_hours::Float64,
)
    if target_runtime_hours <= 0
        return Inf
    end

    return (
        available_energy_wh /
        target_runtime_hours
    )
end


function calculate_power_budget(
    estimate::BatteryStateEstimate,
    desired_runtime_hours::Float64,
)
    raw =
        optimise_power_budget(
            estimate.energy_wh,
            desired_runtime_hours,
        )

    /*
     * Keep a safety reserve.
     */

    return max(
        5.0,
        raw * 0.90,
    )
end


function workload_power_budget(
    battery_soc::Float64,
    on_ac::Bool,
    maximum_power::Float64,
)
    if on_ac
        return maximum_power
    end

    if battery_soc < 10
        return maximum_power * 0.35

    elseif battery_soc < 20
        return maximum_power * 0.50

    elseif battery_soc < 40
        return maximum_power * 0.70

    elseif battery_soc < 60
        return maximum_power * 0.85
    end

    return maximum_power
end
12. JSON API

src/api.jl

using JSON3


function telemetry_from_json(
    input::String,
)
    data =
        JSON3.read(input)

    state =
        if Bool(data.on_ac)
            Charging
        else
            Discharging
        end

    return BatteryTelemetry(
        Float64(data.timestamp),
        Float64(data.voltage),
        Float64(data.current),
        Float64(data.temperature),
        Float64(data.reported_soc),

        state,

        Float64(data.cpu_power),
        Float64(data.gpu_power),
        Float64(data.npu_power),

        Float64(data.display_power),
        Float64(data.memory_power),
        Float64(data.storage_power),
        Float64(data.network_power),
        Float64(data.other_power),

        Bool(data.on_ac),
    )
end


function analyse_json(
    engine::BatteryEngine,
    input::String,
)
    telemetry =
        telemetry_from_json(
            input
        )

    result =
        update!(
            engine,
            telemetry,
        )

    estimate =
        result.estimate

    runtime =
        result.runtime

    return JSON3.write(
        Dict(
            "soc" =>
                estimate.soc,

            "energy_wh" =>
                estimate.energy_wh,

            "power_watts" =>
                estimate.power_watts,

            "temperature" =>
                estimate.temperature,

            "battery_health" =>
                estimate.health,

            "runtime_minutes" =>
                runtime.minutes_remaining,

            "runtime_hours" =>
                runtime.hours_remaining,

            "runtime_confidence" =>
                runtime.confidence,

            "average_power_watts" =>
                result.average_power,
        )
    )
end
13. Main module

src/SurfaceBatteryModel.jl

module SurfaceBatteryModel

include("types.jl")
include("battery.jl")
include("power_model.jl")
include("thermal_model.jl")
include("soc_estimator.jl")
include("runtime_predictor.jl")
include("charging_model.jl")
include("ageing_model.jl")
include("optimizer.jl")
include("api.jl")


export BatteryTelemetry
export BatteryModelParameters
export BatteryStateEstimate
export RuntimePrediction
export ChargingPrediction

export BatteryEngine
export SOCEstimator

export battery_power
export total_system_power
export predict_runtime
export predict_charging
export battery_health
export calculate_power_budget
export workload_power_budget

export update!
export analyse_json

end
14. Demonstration

examples/battery_demo.jl

include(
    "../src/SurfaceBatteryModel.jl"
)

using .SurfaceBatteryModel


parameters =
    BatteryModelParameters(

        15.4,       # nominal voltage

        65.0,       # nominal Wh

        0.92,       # charge efficiency

        0.95,       # discharge efficiency

        2.0,        # idle system power

        0.02,       # thermal coefficient

        1.0,        # ageing factor
    )


engine =
    BatteryEngine(
        parameters,
        initial_soc = 78.0,
        history_size = 120,
    )


telemetry =
    BatteryTelemetry(

        0.0,        # timestamp

        15.2,       # voltage

        1.8,        # current

        34.0,       # battery temperature

        78.0,       # reported SOC

        Discharging,

        8.0,        # CPU

        4.0,        # GPU

        2.0,        # NPU

        4.0,        # display

        2.0,        # memory

        1.0,        # storage

        1.0,        # network

        2.0,        # other

        false,
    )


result =
    update!(
        engine,
        telemetry,
    )


println(
    "================================"
)

println(
    "   SURFACE BATTERY MODEL"
)

println(
    "================================"
)

println(
    "SOC: ",
    result.estimate.soc,
    "%"
)

println(
    "Energy: ",
    result.estimate.energy_wh,
    " Wh"
)

println(
    "Current power: ",
    result.estimate.power_watts,
    " W"
)

println(
    "Average power: ",
    result.average_power,
    " W"
)

println(
    "Runtime: ",
    result.runtime.hours_remaining,
    " hours"
)

println(
    "Confidence: ",
    result.runtime.confidence
)

println(
    "Battery health: ",
    result.estimate.health * 100,
    "%"
)
15. Tests

test/runtests.jl

using Test

include(
    "../src/SurfaceBatteryModel.jl"
)

using .SurfaceBatteryModel


@testset "Battery mathematics" begin

    @test battery_power(
        15.0,
        2.0,
    ) == 30.0


    @test energy_from_soc(
        50.0,
        60.0,
    ) == 30.0


    @test soc_from_energy(
        30.0,
        60.0,
    ) == 50.0
end


@testset "Runtime prediction" begin

    estimate =
        BatteryStateEstimate(
            50.0,
            30.0,
            15.0,
            2.0,
            30.0,
            20.0,
            1.0,
        )

    runtime =
        predict_runtime(
            estimate,
            20.0,
        )

    @test runtime.hours_remaining ≈
        1.5

    @test runtime.minutes_remaining ≈
        90.0
end


@testset "Power budget" begin

    estimate =
        BatteryStateEstimate(
            50.0,
            30.0,
            15.0,
            2.0,
            30.0,
            20.0,
            1.0,
        )

    budget =
        calculate_power_budget(
            estimate,
            1.0,
        )

    @test budget > 0
end


@testset "Battery health" begin

    health =
        battery_health(
            0.0,
            25.0,
        )

    @test health ≈ 1.0
end







Project
SurfaceRadioIntelligence/
│
├── Cargo.toml
│
├── rust/
│   └── src/
│       ├── lib.rs
│       ├── types.rs
│       ├── wifi.rs
│       ├── bluetooth.rs
│       ├── signal.rs
│       ├── connection.rs
│       ├── policy.rs
│       ├── security.rs
│       └── ffi.rs
│
├── cpp/
│   ├── include/
│   │   ├── RadioTypes.hpp
│   │   ├── RustRadioBridge.hpp
│   │   └── SurfaceRadioEngine.hpp
│   │
│   └── src/
│       ├── RustRadioBridge.cpp
│       ├── SurfaceRadioEngine.cpp
│       └── main.cpp
│
└── tests/
    └── radio_tests.rs
1. Rust Cargo.toml
[package]
name = "surface_radio_intelligence"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["rlib", "cdylib"]

[dependencies]
serde = { version = "1", features = ["derive"] }
thiserror = "2"
2. Core radio types

rust/src/types.rs

use serde::{Deserialize, Serialize};

#[derive(
    Debug,
    Clone,
    Copy,
    Serialize,
    Deserialize
)]
pub enum RadioType {
    WiFi,
    Bluetooth,
}

#[derive(
    Debug,
    Clone,
    Copy,
    Serialize,
    Deserialize
)]
pub enum ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Degraded,
}

#[derive(
    Debug,
    Clone,
    Copy,
    Serialize,
    Deserialize
)]
pub struct WiFiTelemetry {
    pub rssi_dbm: f32,
    pub noise_dbm: f32,
    pub channel: u16,
    pub frequency_mhz: u32,
    pub tx_mbps: f32,
    pub rx_mbps: f32,
    pub latency_ms: f32,
    pub packet_loss_percent: f32,
}

#[derive(
    Debug,
    Clone,
    Copy,
    Serialize,
    Deserialize
)]
pub struct BluetoothTelemetry {
    pub rssi_dbm: f32,
    pub connection_interval_ms: f32,
    pub packet_loss_percent: f32,
}

#[derive(
    Debug,
    Clone,
    Copy
)]
pub struct SignalQuality {
    pub score: f32,
    pub margin_db: f32,
    pub stable: bool,
}

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq
)]
pub enum RadioAction {
    Maintain,
    Prefer,
    Reconnect,
    ReducePower,
    IncreaseReliability,
    Defer,
}

#[derive(
    Debug,
    Clone
)]
pub struct RadioPolicy {
    pub wifi_action: RadioAction,
    pub bluetooth_action: RadioAction,

    pub wifi_score: f32,
    pub bluetooth_score: f32,

    pub target_latency_ms: f32,

    pub power_saving: bool,

    pub reason: String,
}
3. Wi-Fi intelligence

rust/src/wifi.rs

use crate::types::*;

pub fn calculate_wifi_quality(
    telemetry: &WiFiTelemetry,
) -> SignalQuality {

    let signal_score =
        ((telemetry.rssi_dbm + 90.0) / 60.0)
            .clamp(0.0, 1.0);

    let noise_margin =
        telemetry.rssi_dbm -
        telemetry.noise_dbm;

    let margin_score =
        (noise_margin / 50.0)
            .clamp(0.0, 1.0);

    let latency_score =
        if telemetry.latency_ms <= 10.0 {
            1.0
        } else if telemetry.latency_ms <= 50.0 {
            0.8
        } else if telemetry.latency_ms <= 100.0 {
            0.5
        } else {
            0.2
        };

    let loss_score =
        (1.0 -
         telemetry.packet_loss_percent / 10.0)
            .clamp(0.0, 1.0);

    let score =
        signal_score * 0.35 +
        margin_score * 0.20 +
        latency_score * 0.25 +
        loss_score * 0.20;

    SignalQuality {
        score,
        margin_db: noise_margin,
        stable:
            telemetry.packet_loss_percent < 2.0 &&
            telemetry.latency_ms < 80.0,
    }
}


pub fn classify_wifi(
    telemetry: &WiFiTelemetry,
) -> RadioAction {

    let quality =
        calculate_wifi_quality(
            telemetry
        );

    if telemetry.packet_loss_percent > 8.0 {
        return RadioAction::Reconnect;
    }

    if telemetry.latency_ms > 150.0 {
        return RadioAction::IncreaseReliability;
    }

    if quality.score < 0.35 {
        return RadioAction::IncreaseReliability;
    }

    if telemetry.rssi_dbm > -50.0 &&
       telemetry.latency_ms < 20.0
    {
        return RadioAction::Prefer;
    }

    RadioAction::Maintain
}
4. Bluetooth intelligence

rust/src/bluetooth.rs

use crate::types::*;

pub fn bluetooth_quality(
    telemetry: &BluetoothTelemetry,
) -> f32 {

    let signal =
        ((telemetry.rssi_dbm + 100.0)
         / 60.0)
        .clamp(0.0, 1.0);

    let loss =
        (
            1.0 -
            telemetry.packet_loss_percent
            / 10.0
        )
        .clamp(0.0, 1.0);

    let interval =
        if telemetry.connection_interval_ms
            <= 15.0
        {
            1.0
        }
        else if telemetry.connection_interval_ms
            <= 50.0
        {
            0.8
        }
        else
        {
            0.6
        };

    signal * 0.45 +
    loss * 0.40 +
    interval * 0.15
}


pub fn classify_bluetooth(
    telemetry: &BluetoothTelemetry,
) -> RadioAction {

    let score =
        bluetooth_quality(
            telemetry
        );

    if telemetry.packet_loss_percent > 10.0 {
        return RadioAction::Reconnect;
    }

    if score < 0.30 {
        return RadioAction::IncreaseReliability;
    }

    if score > 0.80 {
        return RadioAction::Prefer;
    }

    RadioAction::Maintain
}
5. Signal analysis

rust/src/signal.rs

pub fn signal_margin(
    rssi_dbm: f32,
    noise_dbm: f32,
) -> f32 {
    rssi_dbm - noise_dbm
}


pub fn signal_score(
    rssi_dbm: f32,
) -> f32 {

    ((rssi_dbm + 90.0) / 50.0)
        .clamp(0.0, 1.0)
}


pub fn is_weak_signal(
    rssi_dbm: f32,
) -> bool {
    rssi_dbm < -75.0
}


pub fn is_strong_signal(
    rssi_dbm: f32,
) -> bool {
    rssi_dbm > -55.0
}
6. Connection intelligence

rust/src/connection.rs

use crate::types::*;

pub struct ConnectionManager {
    previous_wifi_score: f32,
    previous_bluetooth_score: f32,
}

impl ConnectionManager {

    pub fn new() -> Self {
        Self {
            previous_wifi_score: 0.0,
            previous_bluetooth_score: 0.0,
        }
    }

    pub fn wifi_changed(
        &mut self,
        score: f32,
    ) -> bool {

        let changed =
            (score -
             self.previous_wifi_score)
                .abs() > 0.15;

        self.previous_wifi_score =
            score;

        changed
    }

    pub fn bluetooth_changed(
        &mut self,
        score: f32,
    ) -> bool {

        let changed =
            (score -
             self.previous_bluetooth_score)
                .abs() > 0.15;

        self.previous_bluetooth_score =
            score;

        changed
    }
}
7. Radio policy engine

rust/src/policy.rs

use crate::{
    bluetooth::bluetooth_quality,
    wifi::calculate_wifi_quality,
    types::*,
};


pub struct RadioPolicyEngine;


impl RadioPolicyEngine {

    pub fn evaluate(
        wifi: &WiFiTelemetry,
        bluetooth: &BluetoothTelemetry,
        battery_percent: f32,
    ) -> RadioPolicy {

        let wifi_quality =
            calculate_wifi_quality(
                wifi
            );

        let bluetooth_quality =
            bluetooth_quality(
                bluetooth
            );

        let mut wifi_action =
            crate::wifi::classify_wifi(
                wifi
            );

        let mut bluetooth_action =
            crate::bluetooth::classify_bluetooth(
                bluetooth
            );

        let power_saving =
            battery_percent < 20.0;

        /*
         * Battery-aware networking.
         */

        if power_saving &&
           wifi_quality.score > 0.75
        {
            wifi_action =
                RadioAction::ReducePower;
        }

        if power_saving &&
           bluetooth_quality > 0.80
        {
            bluetooth_action =
                RadioAction::ReducePower;
        }

        /*
         * Poor Wi-Fi should not automatically
         * mean reconnecting. First attempt
         * reliability-oriented behaviour.
         */

        let reason =
            if wifi_quality.score < 0.35 {
                "Poor Wi-Fi radio quality"
            }
            else if wifi.latency_ms > 100.0 {
                "High network latency"
            }
            else if power_saving {
                "Battery-aware radio policy"
            }
            else {
                "Normal radio operation"
            };

        RadioPolicy {
            wifi_action,
            bluetooth_action,

            wifi_score:
                wifi_quality.score,

            bluetooth_score:
                bluetooth_quality,

            target_latency_ms:
                if wifi_quality.score > 0.75 {
                    20.0
                } else {
                    50.0
                },

            power_saving,

            reason:
                reason.to_string(),
        }
    }
}
8. Security layer

The intelligence layer should not make arbitrary trust decisions about nearby networks or Bluetooth devices. Instead, it can enforce conservative state handling.

rust/src/security.rs

use crate::types::*;

#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq
)]
pub enum TrustLevel {
    Unknown,
    Known,
    Trusted,
}


pub struct DeviceTrust {
    pub identifier: String,
    pub radio: RadioType,
    pub trust: TrustLevel,
}


pub fn should_auto_connect(
    trust: TrustLevel,
    previously_connected: bool,
) -> bool {

    match trust {

        TrustLevel::Trusted =>
            true,

        TrustLevel::Known =>
            previously_connected,

        TrustLevel::Unknown =>
            false,
    }
}

This is intentionally conservative: unknown devices aren't automatically trusted simply because their signal is strong.

9. Main Rust intelligence

rust/src/lib.rs

pub mod bluetooth;
pub mod connection;
pub mod policy;
pub mod security;
pub mod signal;
pub mod types;
pub mod wifi;

pub mod ffi;

pub use policy::RadioPolicyEngine;
pub use types::*;
10. C ABI

rust/src/ffi.rs

use crate::{
    policy::RadioPolicyEngine,
    types::{
        BluetoothTelemetry,
        WiFiTelemetry,
    },
};


#[repr(C)]
pub struct CRadioTelemetry {

    pub wifi_rssi: f32,
    pub wifi_noise: f32,

    pub wifi_channel: u16,
    pub wifi_frequency_mhz: u32,

    pub wifi_tx_mbps: f32,
    pub wifi_rx_mbps: f32,

    pub wifi_latency_ms: f32,
    pub wifi_packet_loss: f32,

    pub bluetooth_rssi: f32,

    pub bluetooth_interval_ms: f32,

    pub bluetooth_packet_loss: f32,

    pub battery_percent: f32,
}


#[repr(C)]
pub struct CRadioPolicy {

    pub wifi_action: u32,
    pub bluetooth_action: u32,

    pub wifi_score: f32,
    pub bluetooth_score: f32,

    pub target_latency_ms: f32,

    pub power_saving: bool,
}


#[no_mangle]
pub extern "C"
fn surface_radio_evaluate(
    telemetry: *const CRadioTelemetry,
) -> CRadioPolicy {

    if telemetry.is_null() {

        return CRadioPolicy {
            wifi_action: 0,
            bluetooth_action: 0,
            wifi_score: 0.0,
            bluetooth_score: 0.0,
            target_latency_ms: 100.0,
            power_saving: true,
        };
    }

    let input =
        unsafe { &*telemetry };


    let wifi =
        WiFiTelemetry {
            rssi_dbm:
                input.wifi_rssi,

            noise_dbm:
                input.wifi_noise,

            channel:
                input.wifi_channel,

            frequency_mhz:
                input.wifi_frequency_mhz,

            tx_mbps:
                input.wifi_tx_mbps,

            rx_mbps:
                input.wifi_rx_mbps,

            latency_ms:
                input.wifi_latency_ms,

            packet_loss_percent:
                input.wifi_packet_loss,
        };


    let bluetooth =
        BluetoothTelemetry {
            rssi_dbm:
                input.bluetooth_rssi,

            connection_interval_ms:
                input.bluetooth_interval_ms,

            packet_loss_percent:
                input.bluetooth_packet_loss,
        };


    let policy =
        RadioPolicyEngine::evaluate(
            &wifi,
            &bluetooth,
            input.battery_percent,
        );


    CRadioPolicy {

        wifi_action:
            policy.wifi_action as u32,

        bluetooth_action:
            policy.bluetooth_action as u32,

        wifi_score:
            policy.wifi_score,

        bluetooth_score:
            policy.bluetooth_score,

        target_latency_ms:
            policy.target_latency_ms,

        power_saving:
            policy.power_saving,
    }
}
11. C++ radio types

cpp/include/RadioTypes.hpp

#pragma once

#include <cstdint>


struct SurfaceWiFiState
{
    float rssiDbm{};
    float noiseDbm{};

    std::uint16_t channel{};
    std::uint32_t frequencyMHz{};

    float txMbps{};
    float rxMbps{};

    float latencyMs{};
    float packetLossPercent{};
};


struct SurfaceBluetoothState
{
    float rssiDbm{};

    float connectionIntervalMs{};

    float packetLossPercent{};
};


struct SurfaceRadioTelemetry
{
    SurfaceWiFiState wifi;
    SurfaceBluetoothState bluetooth;

    float batteryPercent{};
};
12. C++ Rust bridge

cpp/include/RustRadioBridge.hpp

#pragma once

#include "RadioTypes.hpp"

#include <cstdint>


struct CRadioTelemetry
{
    float wifi_rssi;
    float wifi_noise;

    std::uint16_t wifi_channel;
    std::uint32_t wifi_frequency_mhz;

    float wifi_tx_mbps;
    float wifi_rx_mbps;

    float wifi_latency_ms;
    float wifi_packet_loss;

    float bluetooth_rssi;
    float bluetooth_interval_ms;
    float bluetooth_packet_loss;

    float battery_percent;
};


struct CRadioPolicy
{
    std::uint32_t wifi_action;
    std::uint32_t bluetooth_action;

    float wifi_score;
    float bluetooth_score;

    float target_latency_ms;

    bool power_saving;
};


extern "C"
{
    CRadioPolicy surface_radio_evaluate(
        const CRadioTelemetry* telemetry);
}


class RustRadioBridge
{
public:

    CRadioPolicy evaluate(
        const SurfaceRadioTelemetry& telemetry
    ) const;
};
13. Bridge implementation

cpp/src/RustRadioBridge.cpp

#include "RustRadioBridge.hpp"


CRadioPolicy
RustRadioBridge::evaluate(
    const SurfaceRadioTelemetry& state
) const
{
    CRadioTelemetry input
    {
        state.wifi.rssiDbm,
        state.wifi.noiseDbm,

        state.wifi.channel,
        state.wifi.frequencyMHz,

        state.wifi.txMbps,
        state.wifi.rxMbps,

        state.wifi.latencyMs,
        state.wifi.packetLossPercent,

        state.bluetooth.rssiDbm,

        state.bluetooth
            .connectionIntervalMs,

        state.bluetooth
            .packetLossPercent,

        state.batteryPercent
    };

    return surface_radio_evaluate(
        &input);
}
14. C++ Surface radio engine

cpp/include/SurfaceRadioEngine.hpp

#pragma once

#include "RustRadioBridge.hpp"

#include <functional>
#include <mutex>


class SurfaceRadioEngine
{
public:

    using PolicyCallback =
        std::function<void(
            const CRadioPolicy&)>;


    void setPolicyCallback(
        PolicyCallback callback);


    CRadioPolicy evaluate(
        const SurfaceRadioTelemetry& telemetry);


private:

    RustRadioBridge bridge_;

    PolicyCallback callback_;

    std::mutex mutex_;
};
15. Engine implementation

cpp/src/SurfaceRadioEngine.cpp

#include "SurfaceRadioEngine.hpp"


void SurfaceRadioEngine::
setPolicyCallback(
    PolicyCallback callback)
{
    std::lock_guard lock(mutex_);

    callback_ =
        std::move(callback);
}


CRadioPolicy
SurfaceRadioEngine::evaluate(
    const SurfaceRadioTelemetry& telemetry)
{
    auto policy =
        bridge_.evaluate(
            telemetry);

    PolicyCallback callback;

    {
        std::lock_guard lock(mutex_);

        callback =
            callback_;
    }

    if (callback)
    {
        callback(policy);
    }

    return policy;
}
16. C++ demonstration

cpp/src/main.cpp

#include "SurfaceRadioEngine.hpp"

#include <iostream>


int main()
{
    SurfaceRadioEngine engine;


    engine.setPolicyCallback(
        [](const CRadioPolicy& policy)
        {
            std::cout
                << "Wi-Fi score: "
                << policy.wifi_score
                << '\n';

            std::cout
                << "Bluetooth score: "
                << policy.bluetooth_score
                << '\n';

            std::cout
                << "Target latency: "
                << policy.target_latency_ms
                << " ms\n";

            std::cout
                << "Power saving: "
                << (
                    policy.power_saving
                    ? "YES"
                    : "NO"
                )
                << "\n";
        });


    SurfaceRadioTelemetry telemetry{};


    telemetry.wifi.rssiDbm =
        -52.0f;

    telemetry.wifi.noiseDbm =
        -92.0f;

    telemetry.wifi.channel =
        36;

    telemetry.wifi.frequencyMHz =
        5180;

    telemetry.wifi.txMbps =
        600.0f;

    telemetry.wifi.rxMbps =
        500.0f;

    telemetry.wifi.latencyMs =
        18.0f;

    telemetry.wifi.packetLossPercent =
        0.5f;


    telemetry.bluetooth.rssiDbm =
        -48.0f;

    telemetry.bluetooth.connectionIntervalMs =
        15.0f;

    telemetry.bluetooth.packetLossPercent =
        0.5f;


    telemetry.batteryPercent =
        72.0f;


    engine.evaluate(
        telemetry);

    return 0;
}
17. Rust tests

tests/radio_tests.rs

use surface_radio_intelligence::{
    bluetooth::{
        bluetooth_quality,
    },
    wifi::{
        calculate_wifi_quality,
    },
    types::*,
};


#[test]
fn strong_wifi_has_good_score()
{
    let wifi =
        WiFiTelemetry {
            rssi_dbm: -45.0,
            noise_dbm: -95.0,

            channel: 36,
            frequency_mhz: 5180,

            tx_mbps: 500.0,
            rx_mbps: 500.0,

            latency_ms: 10.0,
            packet_loss_percent: 0.1,
        };

    let quality =
        calculate_wifi_quality(
            &wifi
        );

    assert!(
        quality.score > 0.8
    );
}


#[test]
fn poor_wifi_is_detected()
{
    let wifi =
        WiFiTelemetry {
            rssi_dbm: -85.0,
            noise_dbm: -90.0,

            channel: 1,
            frequency_mhz: 2412,

            tx_mbps: 5.0,
            rx_mbps: 5.0,

            latency_ms: 250.0,
            packet_loss_percent: 15.0,
        };

    let quality =
        calculate_wifi_quality(
            &wifi
        );

    assert!(
        quality.score < 0.4
    );
}


#[test]
fn bluetooth_score_is_bounded()
{
    let bluetooth =
        BluetoothTelemetry {
            rssi_dbm: -50.0,
            connection_interval_ms: 15.0,
            packet_loss_percent: 0.0,
        };

    let score =
        bluetooth_quality(
            &bluetooth
        );

    assert!(
        score >= 0.0 &&
        score <= 1.0
    );
}
What this gives the Surface

The important thing is that this isn't just a Wi-Fi signal-strength monitor.

The Rust layer can continuously construct a picture of the radio environment:

             RADIO INTELLIGENCE
                     │
       ┌─────────────┼─────────────┐
       ▼             ▼             ▼
     Signal        Latency       Loss rate
       │             │             │
       └─────────────┼─────────────┘
                     ▼
                Quality model
                     │
       ┌─────────────┼──────────────┐
       ▼             ▼              ▼
    Stability     Performance      Battery
       │             │              │
       └─────────────┼──────────────┘
                     ▼
                Rust policy
                     │
             ┌───────┴────────┐
             ▼                ▼
           Wi-Fi          Bluetooth

And it fits particularly nicely with the #16 battery model:

                    Julia
               Battery Model
                     │
                     ▼
               Power budget
                     │
                     ▼
              Rust radio policy
                     │
          ┌──────────┴──────────┐
          ▼                     ▼
        Wi-Fi              Bluetooth
          │                     │
          └──────────┬──────────┘
                     ▼
                    C++
                     │
                     ▼
             Windows networking
             
             
             
             
             
             
             
             
             A sensible architecture is:

Surface Camera
      │
      ▼
Windows Camera / Media Foundation
      │
      ▼
C++ Frame Acquisition
      │
      ├───────────────┐
      ▼               ▼
GPU Preprocessing   Sensor Metadata
      │               │
      └───────┬───────┘
              ▼
       C++ Image Pipeline
              │
      ┌───────┼────────┐
      ▼       ▼        ▼
     HDR   Denoise   Stabilise
      │       │        │
      └───────┼────────┘
              ▼
        Python Models
              │
              ▼
       Model / Parameter
          Selection
              │
              ▼
       C++ GPU Renderer
              │
              ▼
        Final Surface Image
Project
SurfaceComputationalPhotography/
│
├── CMakeLists.txt
├── requirements.txt
│
├── cpp/
│   ├── include/
│   │   ├── CameraTypes.hpp
│   │   ├── Frame.hpp
│   │   ├── Exposure.hpp
│   │   ├── HDRProcessor.hpp
│   │   ├── Denoiser.hpp
│   │   ├── ToneMapper.hpp
│   │   ├── ImagePipeline.hpp
│   │   ├── CameraEngine.hpp
│   │   └── GPUBackend.hpp
│   │
│   └── src/
│       ├── Frame.cpp
│       ├── Exposure.cpp
│       ├── HDRProcessor.cpp
│       ├── Denoiser.cpp
│       ├── ToneMapper.cpp
│       ├── ImagePipeline.cpp
│       ├── CameraEngine.cpp
│       ├── GPUBackend.cpp
│       └── main.cpp
│
├── python/
│   ├── camera_model.py
│   ├── exposure_model.py
│   ├── denoise_model.py
│   ├── hdr_model.py
│   ├── calibration.py
│   └── benchmark.py
│
└── tests/
    └── CameraPipelineTests.cpp
1. Core camera types

cpp/include/CameraTypes.hpp

#pragma once

#include <cstdint>
#include <string>


enum class PixelFormat
{
    RGB8,
    RGBA8,
    BGRA8,
    RGB16,
    FLOAT16,
    FLOAT32
};


enum class CaptureMode
{
    Photo,
    Video,
    LowLight,
    HDR,
    Portrait,
    Document,
    VideoConference
};


struct CameraMetadata
{
    float exposureTimeMs{8.0f};

    float iso{100.0f};

    float aperture{2.0f};

    float whiteBalanceKelvin{5500.0f};

    float focusDistanceMeters{1.0f};

    float ambientLux{300.0f};

    float motionScore{0.0f};

    float faceCount{0.0f};
};


struct CameraConfiguration
{
    std::uint32_t width{1920};

    std::uint32_t height{1080};

    float frameRate{30.0f};

    PixelFormat format{
        PixelFormat::RGBA8
    };

    CaptureMode mode{
        CaptureMode::Photo
    };

    bool hdrEnabled{false};

    bool denoiseEnabled{true};

    bool stabilizationEnabled{true};
};


struct ProcessingParameters
{
    float exposureCompensation{0.0f};

    float denoiseStrength{0.5f};

    float sharpeningStrength{0.25f};

    float toneMapStrength{0.5f};

    float saturation{1.0f};

    float contrast{1.0f};

    bool hdrFusion{false};

    bool temporalDenoise{true};
};


struct CameraPerformance
{
    float captureMs{0.0f};

    float preprocessingMs{0.0f};

    float hdrMs{0.0f};

    float denoiseMs{0.0f};

    float toneMapMs{0.0f};

    float totalMs{0.0f};

    float outputFPS{0.0f};
};
2. Frame representation

cpp/include/Frame.hpp

#pragma once

#include "CameraTypes.hpp"

#include <cstdint>
#include <vector>


class ImageFrame
{
public:

    ImageFrame() = default;


    ImageFrame(
        std::uint32_t width,
        std::uint32_t height,
        PixelFormat format)
        : width_(width),
          height_(height),
          format_(format)
    {
        data_.resize(
            static_cast<std::size_t>(
                width) *
            height *
            channels(format)
        );
    }


    std::uint32_t width() const
    {
        return width_;
    }


    std::uint32_t height() const
    {
        return height_;
    }


    PixelFormat format() const
    {
        return format_;
    }


    std::vector<float>& data()
    {
        return data_;
    }


    const std::vector<float>& data() const
    {
        return data_;
    }


    static std::size_t channels(
        PixelFormat format)
    {
        switch (format)
        {
        case PixelFormat::RGB8:
            return 3;

        case PixelFormat::RGBA8:
            return 4;

        case PixelFormat::BGRA8:
            return 4;

        case PixelFormat::RGB16:
            return 3;

        case PixelFormat::FLOAT16:
            return 4;

        case PixelFormat::FLOAT32:
            return 4;
        }

        return 4;
    }


private:

    std::uint32_t width_{0};

    std::uint32_t height_{0};

    PixelFormat format_{
        PixelFormat::RGBA8
    };

    std::vector<float> data_;
};

This deliberately uses floating-point working buffers. A production GPU implementation would generally avoid unnecessary CPU copies and use GPU-native textures/buffers.

3. Exposure engine

cpp/include/Exposure.hpp

#pragma once

#include "CameraTypes.hpp"


class ExposureController
{
public:

    ProcessingParameters calculate(
        const CameraMetadata& metadata,
        CaptureMode mode) const;
};

cpp/src/Exposure.cpp

#include "Exposure.hpp"

#include <algorithm>


ProcessingParameters
ExposureController::calculate(
    const CameraMetadata& metadata,
    CaptureMode mode) const
{
    ProcessingParameters result;


    if (metadata.ambientLux < 10.0f)
    {
        result.exposureCompensation =
            1.0f;

        result.denoiseStrength =
            0.85f;

        result.temporalDenoise =
            true;
    }
    else if (metadata.ambientLux < 50.0f)
    {
        result.exposureCompensation =
            0.5f;

        result.denoiseStrength =
            0.65f;
    }
    else if (metadata.ambientLux > 1000.0f)
    {
        result.exposureCompensation =
            -0.5f;

        result.denoiseStrength =
            0.25f;
    }


    if (mode == CaptureMode::HDR)
    {
        result.hdrFusion =
            true;

        result.toneMapStrength =
            0.65f;
    }


    if (metadata.motionScore > 0.7f)
    {
        /*
         * Motion makes long exposures dangerous.
         * Reduce temporal processing.
         */
        result.temporalDenoise =
            false;

        result.denoiseStrength =
            std::min(
                result.denoiseStrength,
                0.45f
            );
    }


    return result;
}
4. HDR fusion

cpp/include/HDRProcessor.hpp

#pragma once

#include "Frame.hpp"


class HDRProcessor
{
public:

    ImageFrame fuse(
        const ImageFrame& dark,
        const ImageFrame& normal,
        const ImageFrame& bright) const;
};

cpp/src/HDRProcessor.cpp

#include "HDRProcessor.hpp"

#include <algorithm>


ImageFrame HDRProcessor::fuse(
    const ImageFrame& dark,
    const ImageFrame& normal,
    const ImageFrame& bright) const
{
    if (dark.width() != normal.width() ||
        normal.width() != bright.width() ||
        dark.height() != normal.height() ||
        normal.height() != bright.height())
    {
        return {};
    }


    ImageFrame output(
        normal.width(),
        normal.height(),
        normal.format()
    );


    const auto& d = dark.data();
    const auto& n = normal.data();
    const auto& b = bright.data();

    auto& out = output.data();


    const std::size_t count =
        out.size();


    for (std::size_t i = 0;
         i < count;
         ++i)
    {
        /*
         * Simple exposure fusion.
         *
         * Production version:
         * alignment + motion masks +
         * weighted exposure fusion +
         * ghost removal + GPU execution.
         */

        const float value =
            d[i] * 0.25f +
            n[i] * 0.50f +
            b[i] * 0.25f;


        out[i] =
            std::clamp(
                value,
                0.0f,
                1.0f
            );
    }


    return output;
}

This is the reference CPU implementation. The production version should move this operation to a GPU shader/compute pipeline.

5. Denoising

cpp/include/Denoiser.hpp

#pragma once

#include "Frame.hpp"


class Denoiser
{
public:

    ImageFrame process(
        const ImageFrame& input,
        float strength) const;
};

cpp/src/Denoiser.cpp

#include "Denoiser.hpp"

#include <algorithm>


ImageFrame Denoiser::process(
    const ImageFrame& input,
    float strength) const
{
    ImageFrame output(
        input.width(),
        input.height(),
        input.format()
    );


    const auto& src =
        input.data();

    auto& dst =
        output.data();


    /*
     * Lightweight reference filter.
     *
     * This is intentionally simple.
     * Real Surface implementation should
     * use a GPU bilateral/temporal/AI
     * denoiser.
     */

    const std::size_t count =
        src.size();


    for (std::size_t i = 0;
         i < count;
         ++i)
    {
        const float original =
            src[i];

        const float neighbour =
            i > 0
                ? src[i - 1]
                : original;

        dst[i] =
            original * (1.0f - strength * 0.25f) +
            neighbour * (strength * 0.25f);

        dst[i] =
            std::clamp(
                dst[i],
                0.0f,
                1.0f
            );
    }


    return output;
}
6. Tone mapping

cpp/include/ToneMapper.hpp

#pragma once

#include "Frame.hpp"


class ToneMapper
{
public:

    ImageFrame process(
        const ImageFrame& input,
        float strength,
        float contrast,
        float saturation) const;
};

cpp/src/ToneMapper.cpp

#include "ToneMapper.hpp"

#include <algorithm>
#include <cmath>


ImageFrame ToneMapper::process(
    const ImageFrame& input,
    float strength,
    float contrast,
    float saturation) const
{
    ImageFrame output(
        input.width(),
        input.height(),
        input.format()
    );


    const auto& src =
        input.data();

    auto& dst =
        output.data();


    for (std::size_t i = 0;
         i < src.size();
         ++i)
    {
        float value =
            src[i];


        /*
         * Filmic-style compression.
         */

        value =
            value /
            (value + strength);


        /*
         * Contrast around midpoint.
         */

        value =
            (value - 0.5f)
            * contrast
            + 0.5f;


        value =
            std::clamp(
                value,
                0.0f,
                1.0f
            );


        dst[i] =
            std::pow(
                value,
                1.0f /
                std::max(
                    saturation,
                    0.01f
                )
            );
    }


    return output;
}
7. GPU backend abstraction

cpp/include/GPUBackend.hpp

#pragma once

#include "Frame.hpp"

#include <string>


class GPUBackend
{
public:

    virtual ~GPUBackend() = default;


    virtual bool initialize() = 0;


    virtual bool available() const = 0;


    virtual ImageFrame denoise(
        const ImageFrame& input,
        float strength) = 0;


    virtual ImageFrame hdr(
        const ImageFrame& dark,
        const ImageFrame& normal,
        const ImageFrame& bright) = 0;


    virtual ImageFrame toneMap(
        const ImageFrame& input,
        float strength,
        float contrast,
        float saturation) = 0;
};
8. Reference GPU backend

cpp/src/GPUBackend.cpp

#include "GPUBackend.hpp"

#include "Denoiser.hpp"
#include "HDRProcessor.hpp"
#include "ToneMapper.hpp"


class ReferenceGPUBackend final
    : public GPUBackend
{
public:

    bool initialize() override
    {
        initialized_ = true;
        return true;
    }


    bool available() const override
    {
        return initialized_;
    }


    ImageFrame denoise(
        const ImageFrame& input,
        float strength) override
    {
        return denoiser_.process(
            input,
            strength
        );
    }


    ImageFrame hdr(
        const ImageFrame& dark,
        const ImageFrame& normal,
        const ImageFrame& bright) override
    {
        return hdr_.fuse(
            dark,
            normal,
            bright
        );
    }


    ImageFrame toneMap(
        const ImageFrame& input,
        float strength,
        float contrast,
        float saturation) override
    {
        return toneMapper_.process(
            input,
            strength,
            contrast,
            saturation
        );
    }


private:

    bool initialized_{false};

    Denoiser denoiser_;

    HDRProcessor hdr_;

    ToneMapper toneMapper_;
};

For an actual Surface implementation, this interface can later be backed by Direct3D 12 compute shaders, rather than changing the higher-level camera engine.

9. Complete C++ pipeline

cpp/include/ImagePipeline.hpp

#pragma once

#include "CameraTypes.hpp"
#include "Frame.hpp"
#include "GPUBackend.hpp"


class ImagePipeline
{
public:

    explicit ImagePipeline(
        GPUBackend& gpu);


    ImageFrame process(
        const ImageFrame& input,
        const ProcessingParameters& parameters);


private:

    GPUBackend& gpu_;
};

cpp/src/ImagePipeline.cpp

#include "ImagePipeline.hpp"


ImagePipeline::ImagePipeline(
    GPUBackend& gpu)
    : gpu_(gpu)
{
}


ImageFrame ImagePipeline::process(
    const ImageFrame& input,
    const ProcessingParameters& parameters)
{
    ImageFrame current =
        input;


    if (parameters.denoiseStrength > 0.0f)
    {
        current =
            gpu_.denoise(
                current,
                parameters.denoiseStrength
            );
    }


    current =
        gpu_.toneMap(
            current,
            parameters.toneMapStrength,
            parameters.contrast,
            parameters.saturation
        );


    return current;
}
10. Camera engine

cpp/include/CameraEngine.hpp

#pragma once

#include "CameraTypes.hpp"
#include "Frame.hpp"
#include "Exposure.hpp"
#include "ImagePipeline.hpp"

#include <mutex>
#include <optional>


class CameraEngine
{
public:

    CameraEngine(
        ImagePipeline& pipeline);


    void configure(
        const CameraConfiguration& configuration);


    std::optional<ImageFrame> processFrame(
        const ImageFrame& frame,
        const CameraMetadata& metadata);


    CameraPerformance performance() const;


private:

    CameraConfiguration configuration_;

    ExposureController exposure_;

    ImagePipeline& pipeline_;

    CameraPerformance performance_;

    mutable std::mutex mutex_;
};

cpp/src/CameraEngine.cpp

#include "CameraEngine.hpp"

#include <chrono>


CameraEngine::CameraEngine(
    ImagePipeline& pipeline)
    : pipeline_(pipeline)
{
}


void CameraEngine::configure(
    const CameraConfiguration& configuration)
{
    std::lock_guard lock(mutex_);

    configuration_ =
        configuration;
}


std::optional<ImageFrame>
CameraEngine::processFrame(
    const ImageFrame& frame,
    const CameraMetadata& metadata)
{
    const auto start =
        std::chrono::steady_clock::now();


    CameraConfiguration configuration;

    {
        std::lock_guard lock(mutex_);

        configuration =
            configuration_;
    }


    auto parameters =
        exposure_.calculate(
            metadata,
            configuration.mode
        );


    if (configuration.hdrEnabled)
    {
        parameters.hdrFusion =
            true;
    }


    ImageFrame result =
        pipeline_.process(
            frame,
            parameters
        );


    const auto end =
        std::chrono::steady_clock::now();


    const float elapsed =
        std::chrono::duration<float,
                              std::milli>(
            end - start
        ).count();


    {
        std::lock_guard lock(mutex_);

        performance_.totalMs =
            elapsed;

        performance_.outputFPS =
            elapsed > 0.0f
                ? 1000.0f / elapsed
                : 0.0f;
    }


    return result;
}


CameraPerformance
CameraEngine::performance() const
{
    std::lock_guard lock(mutex_);

    return performance_;
}
11. C++ demo

cpp/src/main.cpp

#include "CameraEngine.hpp"

#include "GPUBackend.cpp"

#include <iostream>


int main()
{
    ReferenceGPUBackend gpu;

    if (!gpu.initialize())
    {
        std::cerr
            << "GPU backend unavailable\n";

        return 1;
    }


    ImagePipeline pipeline(gpu);

    CameraEngine camera(pipeline);


    CameraConfiguration config;

    config.width =
        1920;

    config.height =
        1080;

    config.frameRate =
        30.0f;

    config.mode =
        CaptureMode::Photo;

    config.denoiseEnabled =
        true;

    config.hdrEnabled =
        true;


    camera.configure(config);


    ImageFrame frame(
        config.width,
        config.height,
        config.format
    );


    /*
     * Test image.
     */

    for (auto& pixel :
         frame.data())
    {
        pixel = 0.5f;
    }


    CameraMetadata metadata;

    metadata.ambientLux =
        15.0f;

    metadata.iso =
        800.0f;

    metadata.motionScore =
        0.2f;


    auto output =
        camera.processFrame(
            frame,
            metadata
        );


    if (output)
    {
        auto stats =
            camera.performance();

        std::cout
            << "Processed "
            << output->width()
            << "x"
            << output->height()
            << '\n';

        std::cout
            << "Processing time: "
            << stats.totalMs
            << " ms\n";

        std::cout
            << "Pipeline FPS: "
            << stats.outputFPS
            << '\n';
    }


    return 0;
}

For a real build, ReferenceGPUBackend should be moved into its own header/source rather than including a .cpp file from main.cpp; the above keeps the demonstration compact.

12. Python model development

Now the interesting part.

Python becomes the research laboratory for computational photography.

python/camera_model.py

from dataclasses import dataclass


@dataclass
class CameraFrameStats:
    mean_luminance: float
    noise_estimate: float
    motion_score: float
    dynamic_range: float


@dataclass
class ProcessingRecommendation:
    exposure_compensation: float
    denoise_strength: float
    hdr_strength: float
    sharpening_strength: float


class CameraModel:

    def predict(
        self,
        stats: CameraFrameStats
    ) -> ProcessingRecommendation:

        if stats.mean_luminance < 0.15:
            exposure = 0.8
        elif stats.mean_luminance > 0.85:
            exposure = -0.5
        else:
            exposure = 0.0


        denoise = min(
            max(stats.noise_estimate * 2.0, 0.0),
            1.0
        )


        hdr = min(
            max(
                (stats.dynamic_range - 4.0)
                / 8.0,
                0.0
            ),
            1.0
        )


        sharpening = max(
            0.1,
            0.5 - denoise * 0.3
        )


        return ProcessingRecommendation(
            exposure_compensation=exposure,
            denoise_strength=denoise,
            hdr_strength=hdr,
            sharpening_strength=sharpening,
        )
13. Python exposure model

python/exposure_model.py

import math


def exposure_score(
    mean_luminance: float,
    target: float = 0.45,
) -> float:

    error = target - mean_luminance

    return math.tanh(
        error * 3.0
    )


def recommend_exposure(
    mean_luminance: float,
) -> float:

    score = exposure_score(
        mean_luminance
    )

    return max(
        -2.0,
        min(2.0, score * 2.0)
    )
14. Python denoising model

python/denoise_model.py

from dataclasses import dataclass


@dataclass
class NoiseProfile:
    iso: float
    noise_level: float


def estimate_noise(
    iso: float,
) -> float:

    if iso <= 100:
        return 0.02

    if iso <= 400:
        return 0.08

    if iso <= 800:
        return 0.18

    if iso <= 1600:
        return 0.35

    return 0.55


def denoise_strength(
    iso: float,
    motion_score: float,
) -> float:

    noise = estimate_noise(
        iso
    )

    strength = noise

    if motion_score > 0.7:
        strength *= 0.65

    return max(
        0.0,
        min(1.0, strength)
    )
15. Python HDR model

python/hdr_model.py

from dataclasses import dataclass


@dataclass
class HDRDecision:
    enabled: bool
    exposure_count: int
    fusion_strength: float


def choose_hdr(
    dynamic_range: float,
    motion_score: float,
) -> HDRDecision:

    if motion_score > 0.75:

        return HDRDecision(
            enabled=False,
            exposure_count=1,
            fusion_strength=0.0,
        )


    if dynamic_range < 5.0:

        return HDRDecision(
            enabled=False,
            exposure_count=1,
            fusion_strength=0.0,
        )


    if dynamic_range < 8.0:

        return HDRDecision(
            enabled=True,
            exposure_count=2,
            fusion_strength=0.5,
        )


    return HDRDecision(
        enabled=True,
        exposure_count=3,
        fusion_strength=0.85,
    )

This illustrates an important principle: HDR isn't always desirable. A model can choose not to use multi-frame HDR when motion makes ghosting more likely.

16. Python calibration

python/calibration.py

from dataclasses import dataclass


@dataclass
class CalibrationResult:
    exposure_offset: float
    white_balance_offset: float
    noise_offset: float


def calibrate(
    measured_gray: float,
    target_gray: float,
    measured_temperature: float,
    target_temperature: float,
    measured_noise: float,
    target_noise: float,
) -> CalibrationResult:

    return CalibrationResult(

        exposure_offset =
            target_gray -
            measured_gray,

        white_balance_offset =
            target_temperature -
            measured_temperature,

        noise_offset =
            target_noise -
            measured_noise,
    )
17. Python benchmarking

python/benchmark.py

import statistics
import time


def benchmark(
    function,
    iterations: int = 100,
):
    times = []

    for _ in range(iterations):

        start = time.perf_counter()

        function()

        end = time.perf_counter()

        times.append(
            (end - start) * 1000.0
        )


    return {
        "mean_ms":
            statistics.mean(times),

        "median_ms":
            statistics.median(times),

        "min_ms":
            min(times),

        "max_ms":
            max(times),
    }
18. The model-development loop

This is where Python and C++ complement each other.

              PYTHON
        ┌─────────────────┐
        │ Training data   │
        │ Calibration     │
        │ Model research  │
        │ Parameter search│
        │ Benchmarking    │
        └────────┬────────┘
                 │
                 ▼
          Export parameters
             / model
                 │
                 ▼
              C++
        ┌─────────────────┐
        │ Frame capture   │
        │ GPU processing  │
        │ HDR             │
        │ Denoising       │
        │ Tone mapping    │
        │ Synchronisation │
        └────────┬────────┘
                 │
                 ▼
          Surface camera

Python shouldn't be sitting in the critical frame path if you want a responsive camera. Instead:

Python develops the intelligence → C++ executes it.







Project structure
SurfaceAudioDSP/
├── CMakeLists.txt
├── Cargo.toml
│
├── rust/
│   └── src/
│       ├── lib.rs
│       ├── types.rs
│       ├── ring.rs
│       ├── biquad.rs
│       ├── compressor.rs
│       ├── noise_gate.rs
│       ├── dsp_chain.rs
│       └── ffi.rs
│
├── cpp/
│   ├── include/
│   │   ├── AudioTypes.hpp
│   │   ├── AudioBuffer.hpp
│   │   ├── AudioEngine.hpp
│   │   ├── AudioDevice.hpp
│   │   └── RustDSPBridge.hpp
│   │
│   └── src/
│       ├── AudioBuffer.cpp
│       ├── AudioEngine.cpp
│       ├── AudioDevice.cpp
│       ├── RustDSPBridge.cpp
│       └── main.cpp
│
└── tests/
    ├── dsp_tests.rs
    └── AudioEngineTests.cpp
1. Audio types

cpp/include/AudioTypes.hpp

#pragma once

#include <cstdint>


struct AudioFormat
{
    std::uint32_t sampleRate{48000};

    std::uint16_t channels{2};

    std::uint16_t bitsPerSample{32};

    std::uint32_t framesPerBuffer{128};
};


struct AudioTelemetry
{
    float inputPeak{0.0f};

    float outputPeak{0.0f};

    float rmsLevel{0.0f};

    float cpuLoad{0.0f};

    float latencyMs{0.0f};

    std::uint64_t underruns{0};

    std::uint64_t overruns{0};
};


struct DSPParameters
{
    float masterGain{1.0f};

    float inputGain{1.0f};

    float noiseGateThreshold{0.015f};

    float compressorThreshold{0.7f};

    float compressorRatio{4.0f};

    float attackMs{2.0f};

    float releaseMs{80.0f};

    bool noiseGateEnabled{true};

    bool compressorEnabled{true};

    bool limiterEnabled{true};
};
2. C++ audio buffer

cpp/include/AudioBuffer.hpp

#pragma once

#include "AudioTypes.hpp"

#include <vector>
#include <cstddef>


class AudioBuffer
{
public:

    AudioBuffer() = default;


    AudioBuffer(
        std::size_t frames,
        std::size_t channels)
        : frames_(frames),
          channels_(channels),
          samples_(frames * channels, 0.0f)
    {
    }


    float* data()
    {
        return samples_.data();
    }


    const float* data() const
    {
        return samples_.data();
    }


    std::size_t frames() const
    {
        return frames_;
    }


    std::size_t channels() const
    {
        return channels_;
    }


    float& at(
        std::size_t frame,
        std::size_t channel)
    {
        return samples_[
            frame * channels_ + channel
        ];
    }


    const float& at(
        std::size_t frame,
        std::size_t channel) const
    {
        return samples_[
            frame * channels_ + channel
        ];
    }


private:

    std::size_t frames_{0};

    std::size_t channels_{0};

    std::vector<float> samples_;
};
3. Rust DSP core

rust/src/types.rs

#[repr(C)]
pub struct DspConfig {
    pub sample_rate: f32,
    pub channels: u32,

    pub master_gain: f32,
    pub input_gain: f32,

    pub gate_threshold: f32,

    pub compressor_threshold: f32,
    pub compressor_ratio: f32,

    pub attack_ms: f32,
    pub release_ms: f32,

    pub gate_enabled: u8,
    pub compressor_enabled: u8,
    pub limiter_enabled: u8,
}


#[repr(C)]
pub struct DspTelemetry {
    pub input_peak: f32,
    pub output_peak: f32,
    pub rms: f32,
}

Using u8 instead of C/C++ bool keeps the FFI representation explicit.

4. Rust biquad filter

rust/src/biquad.rs

use std::f32::consts::PI;


pub struct Biquad {
    b0: f32,
    b1: f32,
    b2: f32,

    a1: f32,
    a2: f32,

    x1: f32,
    x2: f32,

    y1: f32,
    y2: f32,
}


impl Biquad {

    pub fn low_pass(
        sample_rate: f32,
        frequency: f32,
        q: f32,
    ) -> Self {

        let omega =
            2.0 * PI * frequency / sample_rate;

        let alpha =
            omega.sin() / (2.0 * q);

        let cos =
            omega.cos();


        let b0 = (1.0 - cos) / 2.0;
        let b1 = 1.0 - cos;
        let b2 = b0;

        let a0 = 1.0 + alpha;
        let a1 = -2.0 * cos;
        let a2 = 1.0 - alpha;


        Self {
            b0: b0 / a0,
            b1: b1 / a0,
            b2: b2 / a0,

            a1: a1 / a0,
            a2: a2 / a0,

            x1: 0.0,
            x2: 0.0,

            y1: 0.0,
            y2: 0.0,
        }
    }


    #[inline]
    pub fn process(
        &mut self,
        input: f32,
    ) -> f32 {

        let output =
            self.b0 * input
            + self.b1 * self.x1
            + self.b2 * self.x2
            - self.a1 * self.y1
            - self.a2 * self.y2;


        self.x2 = self.x1;
        self.x1 = input;

        self.y2 = self.y1;
        self.y1 = output;


        output
    }
}

This is a classic second-order IIR filter, suitable for extremely cheap per-sample processing.

5. Noise gate

rust/src/noise_gate.rs

pub struct NoiseGate {
    threshold: f32,
    enabled: bool,
}


impl NoiseGate {

    pub fn new(
        threshold: f32,
        enabled: bool,
    ) -> Self {

        Self {
            threshold,
            enabled,
        }
    }


    #[inline]
    pub fn process(
        &self,
        sample: f32,
    ) -> f32 {

        if !self.enabled {
            return sample;
        }


        if sample.abs() < self.threshold {
            0.0
        } else {
            sample
        }
    }
}

A production Surface microphone pipeline would normally use a more sophisticated adaptive noise estimator rather than a simple amplitude gate.

6. Compressor

rust/src/compressor.rs

pub struct Compressor {

    threshold: f32,

    ratio: f32,

    attack_coeff: f32,

    release_coeff: f32,

    envelope: f32,

    gain: f32,

    enabled: bool,
}


impl Compressor {

    pub fn new(
        threshold: f32,
        ratio: f32,
        attack_ms: f32,
        release_ms: f32,
        sample_rate: f32,
        enabled: bool,
    ) -> Self {

        let attack_coeff =
            (-1.0 /
                (attack_ms * 0.001 * sample_rate))
                .exp();


        let release_coeff =
            (-1.0 /
                (release_ms * 0.001 * sample_rate))
                .exp();


        Self {
            threshold,
            ratio,

            attack_coeff,
            release_coeff,

            envelope: 0.0,

            gain: 1.0,

            enabled,
        }
    }


    #[inline]
    pub fn process(
        &mut self,
        sample: f32,
    ) -> f32 {

        if !self.enabled {
            return sample;
        }


        let level =
            sample.abs();


        if level > self.envelope {

            self.envelope =
                self.attack_coeff
                * self.envelope
                + (1.0 - self.attack_coeff)
                * level;

        } else {

            self.envelope =
                self.release_coeff
                * self.envelope
                + (1.0 - self.release_coeff)
                * level;
        }


        if self.envelope <= self.threshold {

            self.gain = 1.0;

            return sample;
        }


        let compressed =
            self.threshold
            + (self.envelope - self.threshold)
            / self.ratio;


        self.gain =
            if self.envelope > 0.000001 {
                compressed / self.envelope
            } else {
                1.0
            };


        sample * self.gain
    }
}
7. Rust DSP chain

rust/src/dsp_chain.rs

use crate::compressor::Compressor;
use crate::noise_gate::NoiseGate;
use crate::types::DspConfig;


pub struct DspChain {

    gate: NoiseGate,

    compressor: Compressor,

    master_gain: f32,

    input_gain: f32,

    limiter_enabled: bool,
}


impl DspChain {

    pub fn new(
        config: &DspConfig,
    ) -> Self {

        Self {

            gate: NoiseGate::new(
                config.gate_threshold,
                config.gate_enabled != 0,
            ),

            compressor: Compressor::new(
                config.compressor_threshold,
                config.compressor_ratio,
                config.attack_ms,
                config.release_ms,
                config.sample_rate,
                config.compressor_enabled != 0,
            ),

            master_gain:
                config.master_gain,

            input_gain:
                config.input_gain,

            limiter_enabled:
                config.limiter_enabled != 0,
        }
    }


    #[inline]
    fn limiter(
        &self,
        value: f32,
    ) -> f32 {

        if !self.limiter_enabled {
            return value;
        }


        value.clamp(
            -0.98,
            0.98
        )
    }


    pub fn process(
        &mut self,
        buffer: &mut [f32],
    ) {

        for sample in buffer.iter_mut() {

            let mut value =
                *sample *
                self.input_gain;


            value =
                self.gate.process(
                    value
                );


            value =
                self.compressor.process(
                    value
                );


            value *=
                self.master_gain;


            value =
                self.limiter(value);


            *sample =
                value;
        }
    }
}
8. Rust FFI

rust/src/ffi.rs

use std::slice;

use crate::dsp_chain::DspChain;
use crate::types::{DspConfig, DspTelemetry};


#[repr(C)]
pub struct DspProcessor {
    chain: DspChain,
}


#[no_mangle]
pub extern "C"
fn surface_dsp_create(
    config: *const DspConfig,
) -> *mut DspProcessor {

    if config.is_null() {
        return std::ptr::null_mut();
    }


    let config =
        unsafe {
            &*config
        };


    let processor =
        DspProcessor {
            chain:
                DspChain::new(config),
        };


    Box::into_raw(
        Box::new(processor)
    )
}


#[no_mangle]
pub extern "C"
fn surface_dsp_process(
    processor: *mut DspProcessor,
    samples: *mut f32,
    sample_count: usize,
    telemetry: *mut DspTelemetry,
) {

    if processor.is_null() ||
       samples.is_null() {
        return;
    }


    let processor =
        unsafe {
            &mut *processor
        };


    let buffer =
        unsafe {
            slice::from_raw_parts_mut(
                samples,
                sample_count,
            )
        };


    let mut input_peak =
        0.0f32;

    let mut output_peak =
        0.0f32;

    let mut energy =
        0.0f32;


    for sample in buffer.iter() {

        let value =
            sample.abs();

        input_peak =
            input_peak.max(value);

        energy +=
            sample * sample;
    }


    processor.chain.process(
        buffer
    );


    for sample in buffer.iter() {

        let value =
            sample.abs();

        output_peak =
            output_peak.max(value);

        energy +=
            sample * sample;
    }


    if !telemetry.is_null() {

        unsafe {

            (*telemetry).input_peak =
                input_peak;

            (*telemetry).output_peak =
                output_peak;

            (*telemetry).rms =
                if sample_count > 0 {
                    (energy /
                        sample_count as f32)
                        .sqrt()
                } else {
                    0.0
                };
        }
    }
}


#[no_mangle]
pub extern "C"
fn surface_dsp_destroy(
    processor: *mut DspProcessor,
) {

    if !processor.is_null() {

        unsafe {
            drop(
                Box::from_raw(
                    processor
                )
            );
        }
    }
}

One correction I'd make before production: the telemetry RMS above intentionally measures the combined accumulation in a compact example, but a production implementation should calculate input RMS and output RMS separately.

9. Rust module entry point

rust/src/lib.rs

pub mod types;
pub mod biquad;
pub mod compressor;
pub mod noise_gate;
pub mod dsp_chain;
pub mod ffi;
10. C++ bridge

cpp/include/RustDSPBridge.hpp

#pragma once

#include "AudioTypes.hpp"

#include <cstddef>


extern "C"
{

struct DspConfig
{
    float sampleRate;
    std::uint32_t channels;

    float master_gain;
    float input_gain;

    float gate_threshold;

    float compressor_threshold;
    float compressor_ratio;

    float attack_ms;
    float release_ms;

    std::uint8_t gate_enabled;
    std::uint8_t compressor_enabled;
    std::uint8_t limiter_enabled;
};


struct DspTelemetry
{
    float input_peak;
    float output_peak;
    float rms;
};


struct DspProcessor;


DspProcessor*
surface_dsp_create(
    const DspConfig* config);


void surface_dsp_process(
    DspProcessor* processor,
    float* samples,
    std::size_t sample_count,
    DspTelemetry* telemetry);


void surface_dsp_destroy(
    DspProcessor* processor);

}


class RustDSPBridge
{
public:

    explicit RustDSPBridge(
        const AudioFormat& format,
        const DSPParameters& parameters);


    ~RustDSPBridge();


    RustDSPBridge(
        const RustDSPBridge&) = delete;


    RustDSPBridge& operator=(
        const RustDSPBridge&) = delete;


    bool valid() const;


    void process(
        float* samples,
        std::size_t sampleCount);


    AudioTelemetry telemetry() const;


private:

    DspProcessor* processor_{nullptr};

    AudioTelemetry telemetry_{};
};
11. C++ bridge implementation

cpp/src/RustDSPBridge.cpp

#include "RustDSPBridge.hpp"


RustDSPBridge::RustDSPBridge(
    const AudioFormat& format,
    const DSPParameters& parameters)
{
    DspConfig config{};

    config.sampleRate =
        static_cast<float>(
            format.sampleRate
        );

    config.channels =
        format.channels;

    config.master_gain =
        parameters.masterGain;

    config.input_gain =
        parameters.inputGain;

    config.gate_threshold =
        parameters.noiseGateThreshold;

    config.compressor_threshold =
        parameters.compressorThreshold;

    config.compressor_ratio =
        parameters.compressorRatio;

    config.attack_ms =
        parameters.attackMs;

    config.release_ms =
        parameters.releaseMs;

    config.gate_enabled =
        parameters.noiseGateEnabled ? 1 : 0;

    config.compressor_enabled =
        parameters.compressorEnabled ? 1 : 0;

    config.limiter_enabled =
        parameters.limiterEnabled ? 1 : 0;


    processor_ =
        surface_dsp_create(
            &config
        );
}


RustDSPBridge::~RustDSPBridge()
{
    if (processor_)
    {
        surface_dsp_destroy(
            processor_
        );
    }
}


bool RustDSPBridge::valid() const
{
    return processor_ != nullptr;
}


void RustDSPBridge::process(
    float* samples,
    std::size_t sampleCount)
{
    if (!processor_)
        return;


    DspTelemetry telemetry{};


    surface_dsp_process(
        processor_,
        samples,
        sampleCount,
        &telemetry
    );


    telemetry_.inputPeak =
        telemetry.input_peak;

    telemetry_.outputPeak =
        telemetry.output_peak;

    telemetry_.rmsLevel =
        telemetry.rms;
}


AudioTelemetry
RustDSPBridge::telemetry() const
{
    return telemetry_;
}
12. C++ real-time audio engine

cpp/include/AudioEngine.hpp

#pragma once

#include "AudioTypes.hpp"
#include "AudioBuffer.hpp"
#include "RustDSPBridge.hpp"

#include <memory>


class AudioEngine
{
public:

    AudioEngine(
        const AudioFormat& format,
        const DSPParameters& parameters);


    bool initialize();


    void process(
        AudioBuffer& buffer);


    AudioTelemetry telemetry() const;


private:

    AudioFormat format_;

    std::unique_ptr<RustDSPBridge>
        dsp_;

    AudioTelemetry telemetry_{};

    bool initialized_{false};
};

cpp/src/AudioEngine.cpp

#include "AudioEngine.hpp"

#include <algorithm>
#include <cmath>


AudioEngine::AudioEngine(
    const AudioFormat& format,
    const DSPParameters& parameters)
    : format_(format),
      dsp_(
          std::make_unique<RustDSPBridge>(
              format,
              parameters
          )
      )
{
}


bool AudioEngine::initialize()
{
    if (!dsp_ ||
        !dsp_->valid())
    {
        return false;
    }


    initialized_ =
        true;

    return true;
}


void AudioEngine::process(
    AudioBuffer& buffer)
{
    if (!initialized_)
        return;


    dsp_->process(
        buffer.data(),
        buffer.frames() *
        buffer.channels()
    );


    telemetry_ =
        dsp_->telemetry();


    /*
     * Buffer-level peak calculation.
     */

    float peak =
        0.0f;


    for (std::size_t frame = 0;
         frame < buffer.frames();
         ++frame)
    {
        for (std::size_t channel = 0;
             channel < buffer.channels();
             ++channel)
        {
            peak =
                std::max(
                    peak,
                    std::abs(
                        buffer.at(
                            frame,
                            channel
                        )
                    )
                );
        }
    }


    telemetry_.outputPeak =
        peak;
}


AudioTelemetry
AudioEngine::telemetry() const
{
    return telemetry_;
}
13. Audio device abstraction

cpp/include/AudioDevice.hpp

#pragma once

#include "AudioTypes.hpp"
#include "AudioBuffer.hpp"


class AudioDevice
{
public:

    virtual ~AudioDevice() = default;


    virtual bool open(
        const AudioFormat& format) = 0;


    virtual bool start() = 0;


    virtual bool stop() = 0;


    virtual bool read(
        AudioBuffer& buffer) = 0;


    virtual bool write(
        const AudioBuffer& buffer) = 0;
};

This deliberately separates the DSP engine from Windows audio I/O.

The Windows implementation can then sit behind this interface using the appropriate supported Windows audio APIs.

14. Real-time processing loop

A simplified engine looks like:

void audio_callback(float* samples,
                    std::size_t count)
{
    dsp.process(samples, count);
}

The callback should be designed so that it performs essentially:

INPUT
  │
  ▼
[interleaved samples]
  │
  ▼
Rust DSP
  │
  ├── gain
  ├── gate
  ├── compressor
  ├── limiter
  └── filters
  │
  ▼
OUTPUT

No:

❌ malloc
❌ mutex
❌ file I/O
❌ logging
❌ network call
❌ Python
❌ unpredictable blocking

inside that callback.

15. Rust tests

tests/dsp_tests.rs

use surface_audio_dsp::types::DspConfig;
use surface_audio_dsp::dsp_chain::DspChain;


#[test]
fn limiter_prevents_large_output()
{
    let config =
        DspConfig {
            sample_rate: 48000.0,
            channels: 2,

            master_gain: 2.0,
            input_gain: 2.0,

            gate_threshold: 0.0,

            compressor_threshold: 0.7,
            compressor_ratio: 4.0,

            attack_ms: 2.0,
            release_ms: 80.0,

            gate_enabled: 0,
            compressor_enabled: 0,
            limiter_enabled: 1,
        };


    let mut chain =
        DspChain::new(
            &config
        );


    let mut samples =
        vec![1.0f32; 128];


    chain.process(
        &mut samples
    );


    for sample in samples {
        assert!(
            sample.abs() <= 0.98
        );
    }
}
16. C++ test

tests/AudioEngineTests.cpp

#include "AudioEngine.hpp"

#include <cassert>
#include <cmath>


int main()
{
    AudioFormat format;

    format.sampleRate =
        48000;

    format.channels =
        2;

    format.framesPerBuffer =
        128;


    DSPParameters parameters;

    parameters.masterGain =
        1.0f;

    parameters.inputGain =
        1.0f;

    parameters.limiterEnabled =
        true;


    AudioEngine engine(
        format,
        parameters
    );


    assert(
        engine.initialize()
    );


    AudioBuffer buffer(
        128,
        2
    );


    for (std::size_t i = 0;
         i < 256;
         ++i)
    {
        buffer.data()[i] =
            0.5f;
    }


    engine.process(
        buffer
    );


    for (std::size_t i = 0;
         i < 256;
         ++i)
    {
        assert(
            std::isfinite(
                buffer.data()[i]
            )
        );
    }


    return 0;
}
17. Cargo configuration

Cargo.toml

[package]
name = "surface_audio_dsp"
version = "0.1.0"
edition = "2021"

[lib]
name = "surface_audio_dsp"
crate-type = ["rlib", "cdylib"]

[profile.release]
opt-level = 3
lto = true
codegen-units = 1
panic = "abort"
18. CMake

CMakeLists.txt

cmake_minimum_required(VERSION 3.24)

project(
    SurfaceAudioDSP
    LANGUAGES CXX
)

set(
    CMAKE_CXX_STANDARD 20
)

set(
    CMAKE_CXX_STANDARD_REQUIRED ON
)


add_library(
    SurfaceAudio
    STATIC

    cpp/src/AudioBuffer.cpp
    cpp/src/AudioEngine.cpp
    cpp/src/AudioDevice.cpp
    cpp/src/RustDSPBridge.cpp
)


target_include_directories(
    SurfaceAudio
    PUBLIC
    cpp/include
)


add_executable(
    SurfaceAudioDemo
    cpp/src/main.cpp
)


target_link_libraries(
    SurfaceAudioDemo
    PRIVATE
    SurfaceAudio
)

The final production build should add a CMake custom target that invokes Cargo and then links the generated Windows Rust library/DLL import library into the C++ target.

19. Demo

cpp/src/main.cpp

#include "AudioEngine.hpp"

#include <iostream>


int main()
{
    AudioFormat format;

    format.sampleRate = 48000;

    format.channels = 2;

    format.framesPerBuffer = 128;


    DSPParameters parameters;

    parameters.masterGain =
        1.0f;

    parameters.inputGain =
        1.0f;

    parameters.noiseGateEnabled =
        true;

    parameters.compressorEnabled =
        true;

    parameters.limiterEnabled =
        true;


    AudioEngine engine(
        format,
        parameters
    );


    if (!engine.initialize())
    {
        std::cerr
            << "Audio engine initialization failed\n";

        return 1;
    }


    AudioBuffer buffer(
        format.framesPerBuffer,
        format.channels
    );


    /*
     * Simulated microphone signal.
     */

    for (std::size_t i = 0;
         i < buffer.frames();
         ++i)
    {
        float sample =
            0.25f;

        for (std::size_t c = 0;
             c < buffer.channels();
             ++c)
        {
            buffer.at(i, c) =
                sample;
        }
    }


    engine.process(
        buffer
    );


    const auto telemetry =
        engine.telemetry();


    std::cout
        << "Output peak: "
        << telemetry.outputPeak
        << '\n';


    std::cout
        << "RMS: "
        << telemetry.rmsLevel
        << '\n';


    return 0;
}








Project structure
SurfaceCloudSync/
│
├── go.mod
├── cmd/
│   └── surfacesync/
│       └── main.go
│
├── internal/
│   ├── model/
│   │   └── types.go
│   ├── journal/
│   │   └── journal.go
│   ├── device/
│   │   └── registry.go
│   ├── sync/
│   │   ├── engine.go
│   │   ├── scheduler.go
│   │   └── conflict.go
│   ├── transfer/
│   │   └── chunks.go
│   ├── crypto/
│   │   └── crypto.go
│   ├── transport/
│   │   └── http.go
│   └── telemetry/
│       └── telemetry.go
│
└── tests/
    └── sync_test.go
1. Go module

go.mod

module surfacesync

go 1.24

For a production implementation, cryptographic and protocol dependencies should be pinned and audited rather than casually added.

2. Core data model

internal/model/types.go

package model

import "time"

type DeviceID string

type FileID string

type OperationType uint8

const (
	OperationCreate OperationType = iota
	OperationUpdate
	OperationDelete
	OperationRename
)

type Device struct {
	ID          DeviceID
	Name        string
	LastSeen    time.Time
	Online      bool
	OSVersion   string
	SyncVersion uint64
}

type FileMetadata struct {
	ID          FileID
	Path        string
	Size        int64
	ModifiedAt  time.Time
	ContentHash string
	Version     uint64
	Deleted     bool
}

type Change struct {
	ID         string
	DeviceID   DeviceID
	File       FileMetadata
	Operation  OperationType
	Timestamp  time.Time
	Sequence   uint64
}

type SyncCursor struct {
	DeviceID DeviceID
	Sequence uint64
}

type SyncRequest struct {
	DeviceID DeviceID
	Cursor   SyncCursor
}

type SyncResponse struct {
	Changes []Change
	Cursor  SyncCursor
}

type Conflict struct {
	FileID       FileID
	LocalVersion FileMetadata
	RemoteVersion FileMetadata
	DetectedAt   time.Time
}

type SyncResult struct {
	Uploaded int
	Downloaded int
	Conflicts int
	Deleted int
}

The sequence number is important. Synchronization should not depend purely on timestamps.

3. Change journal

internal/journal/journal.go

package journal

import (
	"sync"

	"surfacesync/internal/model"
)

type Journal struct {
	mu       sync.RWMutex
	changes  []model.Change
	sequence uint64
}

func New() *Journal {
	return &Journal{
		changes: make([]model.Change, 0),
	}
}

func (j *Journal) Append(change model.Change) model.Change {
	j.mu.Lock()
	defer j.mu.Unlock()

	j.sequence++

	change.Sequence = j.sequence

	j.changes = append(
		j.changes,
		change,
	)

	return change
}

func (j *Journal) Since(sequence uint64) []model.Change {
	j.mu.RLock()
	defer j.mu.RUnlock()

	result := make(
		[]model.Change,
		0,
	)

	for _, change := range j.changes {
		if change.Sequence > sequence {
			result = append(
				result,
				change,
			)
		}
	}

	return result
}

func (j *Journal) LatestSequence() uint64 {
	j.mu.RLock()
	defer j.mu.RUnlock()

	return j.sequence
}

This is an in-memory implementation. A real service would persist the journal, probably using an append-only local database/log.

4. Device registry

internal/device/registry.go

package device

import (
	"sync"
	"time"

	"surfacesync/internal/model"
)

type Registry struct {
	mu      sync.RWMutex
	devices map[model.DeviceID]model.Device
}

func NewRegistry() *Registry {
	return &Registry{
		devices: make(
			map[model.DeviceID]model.Device,
		),
	}
}

func (r *Registry) Register(
	d model.Device,
) {
	r.mu.Lock()
	defer r.mu.Unlock()

	r.devices[d.ID] = d
}

func (r *Registry) Heartbeat(
	id model.DeviceID,
) {
	r.mu.Lock()
	defer r.mu.Unlock()

	device, exists :=
		r.devices[id]

	if !exists {
		return
	}

	device.LastSeen = time.Now()
	device.Online = true

	r.devices[id] = device
}

func (r *Registry) Get(
	id model.DeviceID,
) (model.Device, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	device, exists :=
		r.devices[id]

	return device, exists
}

func (r *Registry) List() []model.Device {
	r.mu.RLock()
	defer r.mu.RUnlock()

	result :=
		make([]model.Device, 0, len(r.devices))

	for _, device :=
		range r.devices {

		result = append(
			result,
			device,
		)
	}

	return result
}

Go's concurrency model makes this kind of device registry straightforward.

5. Conflict resolution

internal/sync/conflict.go

package sync

import (
	"errors"
	"time"

	"surfacesync/internal/model"
)

var ErrConflict = errors.New(
	"file synchronization conflict",
)

type ConflictResolver struct{}

func NewConflictResolver() *ConflictResolver {
	return &ConflictResolver{}
}

func (r *ConflictResolver) Resolve(
	local model.FileMetadata,
	remote model.FileMetadata,
) (model.FileMetadata, error) {

	if local.ContentHash ==
		remote.ContentHash {

		if remote.Version >= local.Version {
			return remote, nil
		}

		return local, nil
	}

	if remote.Version > local.Version {
		return remote, ErrConflict
	}

	if local.Version > remote.Version {
		return local, ErrConflict
	}

	/*
	 * Same logical version but different content.
	 * This must not silently overwrite either copy.
	 */

	return model.FileMetadata{
		ID:          local.ID,
		Path:        local.Path,
		Size:        local.Size,
		ModifiedAt:  time.Now(),
		ContentHash: "",
		Version:    local.Version + 1,
	}, ErrConflict
}

For documents, a future implementation could use application-specific merge semantics. For arbitrary binary files, automatic merging is generally unsafe.

6. Chunked transfers

Large Surface files shouldn't be transferred as one giant request.

internal/transfer/chunks.go

package transfer

import (
	"crypto/sha256"
	"encoding/hex"
)

const DefaultChunkSize = 4 * 1024 * 1024

type Chunk struct {
	Index uint32
	Data  []byte
	Hash  string
}

func Split(
	data []byte,
	chunkSize int,
) []Chunk {

	if chunkSize <= 0 {
		chunkSize =
			DefaultChunkSize
	}

	var chunks []Chunk

	for offset := 0;
		offset < len(data);
		offset += chunkSize {

		end :=
			offset + chunkSize

		if end > len(data) {
			end = len(data)
		}

		part :=
			data[offset:end]

		hash :=
			sha256.Sum256(part)

		chunks =
			append(
				chunks,
				Chunk{
					Index: uint32(
						len(chunks),
					),
					Data: part,
					Hash: hex.EncodeToString(
						hash[:],
					),
				},
			)
	}

	return chunks
}

This gives us resumability later:

4 GB file

Chunk 0 ✓
Chunk 1 ✓
Chunk 2 ✓
Chunk 3 ✓
Chunk 4 ✗

             ↓ reconnect

Resume at Chunk 4
7. Cryptographic integrity

internal/crypto/crypto.go

package crypto

import (
	"crypto/sha256"
	"encoding/hex"
)

func SHA256(
	data []byte,
) string {

	hash :=
		sha256.Sum256(data)

	return hex.EncodeToString(
		hash[:],
	)
}

func Verify(
	data []byte,
	expected string,
) bool {

	actual :=
		SHA256(data)

	return actual == expected
}

Hashing establishes content integrity, but it isn't authentication. Production synchronization should additionally use authenticated TLS and authenticated device identity.

8. HTTP transport

internal/transport/http.go

package transport

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"surfacesync/internal/model"
)

type Client struct {
	baseURL    string
	httpClient *http.Client
}

func NewClient(
	baseURL string,
) *Client {

	return &Client{
		baseURL: baseURL,

		httpClient: &http.Client{
			Timeout: 30 * time.Second,
		},
	}
}

func (c *Client) PullChanges(
	ctx context.Context,
	request model.SyncRequest,
) (model.SyncResponse, error) {

	body, err :=
		json.Marshal(request)

	if err != nil {
		return model.SyncResponse{}, err
	}

	req, err :=
		http.NewRequestWithContext(
			ctx,
			http.MethodPost,
			c.baseURL+"/sync/pull",
			bytesReader(body),
		)

	if err != nil {
		return model.SyncResponse{}, err
	}

	req.Header.Set(
		"Content-Type",
		"application/json",
	)


	response, err :=
		c.httpClient.Do(req)

	if err != nil {
		return model.SyncResponse{}, err
	}

	defer response.Body.Close()


	if response.StatusCode !=
		http.StatusOK {

		return model.SyncResponse{},
			fmt.Errorf(
				"sync server returned %s",
				response.Status,
			)
	}


	var result model.SyncResponse

	if err :=
		json.NewDecoder(
			response.Body,
		).Decode(&result);
		err != nil {

		return model.SyncResponse{}, err
	}


	return result, nil
}

We need the reader helper:

package transport

import "bytes"

func bytesReader(
	data []byte,
) *bytes.Reader {
	return bytes.NewReader(data)
}

Put that in internal/transport/reader.go.

9. Synchronisation scheduler

internal/sync/scheduler.go

package sync

import (
	"context"
	"sync"
	"time"
)

type Scheduler struct {
	interval time.Duration
	tasks    chan func(context.Context)
}

func NewScheduler(
	workers int,
	interval time.Duration,
) *Scheduler {

	s :=
		&Scheduler{
			interval: interval,
			tasks: make(
				chan func(context.Context),
				64,
			),
		}

	for i := 0;
		i < workers;
		i++ {

		go s.worker()
		}

	return s
}

func (s *Scheduler) worker() {

	for task := range s.tasks {

		ctx, cancel :=
			context.WithTimeout(
				context.Background(),
				2*time.Minute,
			)

		task(ctx)

		cancel()
	}
}

func (s *Scheduler) Submit(
	task func(context.Context),
) {
	s.tasks <- task
}

func (s *Scheduler) Run(
	ctx context.Context,
	task func(context.Context),
) {

	ticker :=
		time.NewTicker(
			s.interval,
		)

	defer ticker.Stop()


	for {

		select {

		case <-ticker.C:
			s.Submit(task)

		case <-ctx.Done():
			return
		}
	}
}

var _ = sync.Once{}

The unused sync import isn't actually required; the production version should remove it:

import (
    "context"
    "time"
)
10. Synchronisation engine

internal/sync/engine.go

package sync

import (
	"context"
	"sync"
	"time"

	"surfacesync/internal/journal"
	"surfacesync/internal/model"
)

type Engine struct {
	deviceID model.DeviceID

	journal *journal.Journal

	resolver *ConflictResolver

	mu sync.Mutex

	cursor uint64
}

func NewEngine(
	deviceID model.DeviceID,
	j *journal.Journal,
) *Engine {

	return &Engine{
		deviceID: deviceID,
		journal:  j,
		resolver: NewConflictResolver(),
	}
}

func (e *Engine) LocalChange(
	change model.Change,
) model.Change {

	change.DeviceID =
		e.deviceID

	change.Timestamp =
		time.Now()

	return e.journal.Append(
		change,
	)
}

func (e *Engine) PendingChanges()
	[]model.Change {

	e.mu.Lock()
	defer e.mu.Unlock()

	return e.journal.Since(
		e.cursor,
	)
}

func (e *Engine) CommitCursor(
	sequence uint64,
) {

	e.mu.Lock()
	defer e.mu.Unlock()

	if sequence > e.cursor {
		e.cursor = sequence
	}
}

func (e *Engine) Synchronize(
	ctx context.Context,
	remote []model.Change,
) model.SyncResult {

	result :=
		model.SyncResult{}


	for _, change :=
		range remote {

		select {

		case <-ctx.Done():
			return result

		default:
		}


		result.Downloaded++
	}


	return result
}
11. Main service

cmd/surfacesync/main.go

package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"surfacesync/internal/device"
	"surfacesync/internal/journal"
	"sync"
	"surfacesync/internal/model"
)

func main() {

	ctx, cancel :=
		signal.NotifyContext(
			context.Background(),
			os.Interrupt,
			syscall.SIGTERM,
		)

	defer cancel()


	registry :=
		device.NewRegistry()

	journal :=
		journal.New()


	deviceID :=
		model.DeviceID(
			"surface-device-001",
		)


	registry.Register(
		model.Device{
			ID:        deviceID,
			Name:      "Surface Development Device",
			LastSeen:  time.Now(),
			Online:    true,
			OSVersion: "Windows",
		},
	)


	var workers sync.WaitGroup

	for i := 0; i < 4; i++ {

		workers.Add(1)

		go func(worker int) {

			defer workers.Done()

			<-ctx.Done()

			fmt.Printf(
				"sync worker %d stopped\n",
				worker,
			)

		}(i)
	}


	fmt.Println(
		"Surface Cloud Sync Service started",
	)


	ticker :=
		time.NewTicker(
			10 * time.Second,
		)

	defer ticker.Stop()


	for {

		select {

		case <-ticker.C:

			registry.Heartbeat(
				deviceID,
			)

			fmt.Printf(
				"heartbeat: %s\n",
				time.Now().
					Format(time.RFC3339),
			)


		case <-ctx.Done():

			workers.Wait()

			fmt.Println(
				"Surface Cloud Sync Service stopped",
			)

			return
		}
	}

	_ = journal
}
12. Tests

tests/sync_test.go

package tests

import (
	"testing"

	"surfacesync/internal/crypto"
	"surfacesync/internal/transfer"
)

func TestSHA256(t *testing.T) {

	hash :=
		crypto.SHA256(
			[]byte("surface"),
		)

	if hash == "" {
		t.Fatal(
			"expected hash",
		)
	}
}


func TestVerify(t *testing.T) {

	data :=
		[]byte("surface cloud")


	hash :=
		crypto.SHA256(data)


	if !crypto.Verify(
		data,
		hash,
	) {
		t.Fatal(
			"hash verification failed",
		)
	}
}


func TestChunking(t *testing.T) {

	data :=
		make([]byte, 10)


	chunks :=
		transfer.Split(
			data,
			4,
		)


	if len(chunks) != 3 {

		t.Fatalf(
			"expected 3 chunks, got %d",
			len(chunks),
		)
	}


	if chunks[0].Index != 0 {
		t.Fatal(
			"incorrect first chunk index",
		)
	}


	if len(chunks[2].Data) != 2 {
		t.Fatal(
			"incorrect final chunk size",
		)
	}
}
13. The real synchronisation model

The important part isn't simply uploading files. It's maintaining a distributed state machine:

                 CLOUD
                   │
          ┌────────┴────────┐
          │ Change Journal  │
          │ Device Registry │
          │ Object Storage  │
          └────────┬────────┘
                   │
          ┌────────┼────────┐
          ▼        ▼        ▼
       Surface   Surface   Surface
        Laptop    Pro      Tablet
          │        │        │
          ▼        ▼        ▼
       Journal  Journal  Journal
          │        │        │
          └────────┼────────┘
                   ▼
             Reconciliation

Each device maintains:

Device ID
     +
Sync cursor
     +
Change sequence
     +
Content hashes
     +
Version numbers

That allows the service to determine:

What changed?
       ↓
Where did it change?
       ↓
Has this device already received it?
       ↓
Is the content identical?
       ↓
Is there a conflict?
       ↓
Can it be merged?
       ↓
If not → preserve both versions
14. Concurrent synchronisation

Go's goroutines become particularly useful here.

For example:

go syncDevice(
    laptop,
)

go syncDevice(
    tablet,
)

go syncDevice(
    studioPC,
)

go syncDevice(
    phone,
)






Project
SurfaceDeviceServices/
├── Cargo.toml
├── README.md
├── config/
│   └── default.toml
├── src/
│   ├── main.rs
│   ├── lib.rs
│   │
│   ├── config.rs
│   ├── error.rs
│   ├── types.rs
│   ├── state.rs
│   │
│   ├── runtime/
│   │   ├── mod.rs
│   │   ├── supervisor.rs
│   │   ├── worker.rs
│   │   └── shutdown.rs
│   │
│   ├── services/
│   │   ├── mod.rs
│   │   ├── device.rs
│   │   ├── power.rs
│   │   ├── thermal.rs
│   │   ├── radio.rs
│   │   ├── display.rs
│   │   └── health.rs
│   │
│   ├── health/
│   │   ├── mod.rs
│   │   ├── monitor.rs
│   │   └── watchdog.rs
│   │
│   ├── events/
│   │   ├── mod.rs
│   │   ├── event.rs
│   │   └── journal.rs
│   │
│   ├── ipc/
│   │   ├── mod.rs
│   │   └── protocol.rs
│   │
│   └── windows/
│       ├── mod.rs
│       ├── service.rs
│       └── adapters.rs
│
└── tests/
    ├── runtime_tests.rs
    ├── health_tests.rs
    └── event_tests.rs
Cargo.toml
[package]
name = "surface_device_services"
version = "0.1.0"
edition = "2021"

[lib]
name = "surface_device_services"
path = "src/lib.rs"

[[bin]]
name = "surface-device-service"
path = "src/main.rs"

[dependencies]
anyhow = "1"
async-trait = "0.1"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
thiserror = "2"
tokio = { version = "1", features = [
    "macros",
    "rt-multi-thread",
    "signal",
    "sync",
    "time"
] }
toml = "0.9"
uuid = { version = "1", features = ["v4", "serde"] }
tracing = "0.1"
tracing-subscriber = { version = "0.3", features = ["fmt", "env-filter"] }

[target.'cfg(windows)'.dependencies]
windows = { version = "0.61", features = [
    "Win32_System_Services",
    "Win32_System_Threading"
] }

[dev-dependencies]
tokio = { version = "1", features = ["macros", "rt-multi-thread", "time"] }
src/lib.rs
pub mod config;
pub mod error;
pub mod events;
pub mod health;
pub mod ipc;
pub mod runtime;
pub mod services;
pub mod state;
pub mod types;
pub mod windows;
src/error.rs
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ServiceError {
    #[error("service initialization failed: {0}")]
    Initialization(String),

    #[error("service stopped: {0}")]
    Stopped(String),

    #[error("service timeout")]
    Timeout,

    #[error("invalid configuration: {0}")]
    Configuration(String),

    #[error("IPC failure: {0}")]
    Ipc(String),

    #[error("device adapter failure: {0}")]
    DeviceAdapter(String),

    #[error("internal error: {0}")]
    Internal(String),
}

pub type ServiceResult<T> = Result<T, ServiceError>;
src/types.rs

This is the common language shared by all Surface services.

use serde::{Deserialize, Serialize};
use std::time::SystemTime;
use uuid::Uuid;

pub type ServiceId = Uuid;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum ServiceState {
    Created,
    Starting,
    Running,
    Degraded,
    Stopping,
    Stopped,
    Failed,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum HealthState {
    Unknown,
    Healthy,
    Degraded,
    Unhealthy,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServiceStatus {
    pub id: ServiceId,
    pub name: String,
    pub state: ServiceState,
    pub health: HealthState,
    pub restart_count: u32,
    pub last_heartbeat: Option<SystemTime>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceIdentity {
    pub device_id: Uuid,
    pub model: String,
    pub firmware_version: String,
    pub os_build: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceSnapshot {
    pub identity: DeviceIdentity,
    pub battery_percent: f32,
    pub temperature_celsius: f32,
    pub on_ac_power: bool,
    pub network_connected: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HealthReport {
    pub state: HealthState,
    pub uptime_seconds: u64,
    pub active_services: usize,
    pub failed_services: usize,
}
src/config.rs
use serde::Deserialize;
use std::time::Duration;

#[derive(Debug, Clone, Deserialize)]
pub struct Config {
    #[serde(default)]
    pub runtime: RuntimeConfig,

    #[serde(default)]
    pub health: HealthConfig,

    #[serde(default)]
    pub events: EventConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct RuntimeConfig {
    #[serde(default = "default_heartbeat")]
    pub heartbeat_seconds: u64,

    #[serde(default = "default_shutdown")]
    pub shutdown_timeout_seconds: u64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct HealthConfig {
    #[serde(default = "default_health_interval")]
    pub interval_seconds: u64,

    #[serde(default = "default_watchdog_timeout")]
    pub watchdog_timeout_seconds: u64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct EventConfig {
    #[serde(default = "default_event_capacity")]
    pub capacity: usize,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            heartbeat_seconds: default_heartbeat(),
            shutdown_timeout_seconds: default_shutdown(),
        }
    }
}

impl Default for HealthConfig {
    fn default() -> Self {
        Self {
            interval_seconds: default_health_interval(),
            watchdog_timeout_seconds: default_watchdog_timeout(),
        }
    }
}

impl Default for EventConfig {
    fn default() -> Self {
        Self {
            capacity: default_event_capacity(),
        }
    }
}

impl Config {
    pub fn heartbeat_interval(&self) -> Duration {
        Duration::from_secs(self.runtime.heartbeat_seconds)
    }

    pub fn health_interval(&self) -> Duration {
        Duration::from_secs(self.health.interval_seconds)
    }

    pub fn watchdog_timeout(&self) -> Duration {
        Duration::from_secs(self.health.watchdog_timeout_seconds)
    }
}

fn default_heartbeat() -> u64 {
    5
}

fn default_shutdown() -> u64 {
    10
}

fn default_health_interval() -> u64 {
    5
}

fn default_watchdog_timeout() -> u64 {
    15
}

fn default_event_capacity() -> usize {
    4096
}
src/state.rs

Global runtime state should be centralized rather than scattered across individual services.

use crate::types::{DeviceSnapshot, ServiceStatus};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::RwLock;
use uuid::Uuid;

#[derive(Clone)]
pub struct RuntimeState {
    inner: Arc<RwLock<RuntimeStateInner>>,
}

struct RuntimeStateInner {
    device: Option<DeviceSnapshot>,
    services: HashMap<Uuid, ServiceStatus>,
}

impl RuntimeState {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(RwLock::new(RuntimeStateInner {
                device: None,
                services: HashMap::new(),
            })),
        }
    }

    pub async fn set_device(&self, snapshot: DeviceSnapshot) {
        self.inner.write().await.device = Some(snapshot);
    }

    pub async fn device(&self) -> Option<DeviceSnapshot> {
        self.inner.read().await.device.clone()
    }

    pub async fn update_service(&self, status: ServiceStatus) {
        self.inner
            .write()
            .await
            .services
            .insert(status.id, status);
    }

    pub async fn services(&self) -> Vec<ServiceStatus> {
        self.inner
            .read()
            .await
            .services
            .values()
            .cloned()
            .collect()
    }
}
Event system
src/events/event.rs
use serde::{Deserialize, Serialize};
use std::time::SystemTime;
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum EventSeverity {
    Debug,
    Info,
    Warning,
    Error,
    Critical,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceEvent {
    pub id: Uuid,
    pub timestamp: SystemTime,
    pub source: String,
    pub severity: EventSeverity,
    pub message: String,
}

impl DeviceEvent {
    pub fn info(source: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            id: Uuid::new_v4(),
            timestamp: SystemTime::now(),
            source: source.into(),
            severity: EventSeverity::Info,
            message: message.into(),
        }
    }
}
src/events/journal.rs
use super::event::DeviceEvent;
use std::collections::VecDeque;
use tokio::sync::RwLock;

pub struct EventJournal {
    capacity: usize,
    events: RwLock<VecDeque<DeviceEvent>>,
}

impl EventJournal {
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            events: RwLock::new(VecDeque::with_capacity(capacity)),
        }
    }

    pub async fn append(&self, event: DeviceEvent) {
        let mut events = self.events.write().await;

        if events.len() >= self.capacity {
            events.pop_front();
        }

        events.push_back(event);
    }

    pub async fn recent(&self, count: usize) -> Vec<DeviceEvent> {
        let events = self.events.read().await;

        events
            .iter()
            .rev()
            .take(count)
            .cloned()
            .collect()
    }

    pub async fn len(&self) -> usize {
        self.events.read().await.len()
    }
}
src/events/mod.rs
pub mod event;
pub mod journal;

pub use event::*;
pub use journal::*;
Service abstraction
src/services/mod.rs
pub mod device;
pub mod display;
pub mod health;
pub mod power;
pub mod radio;
pub mod thermal;

use crate::{
    error::ServiceResult,
    types::{ServiceId, ServiceStatus},
};
use async_trait::async_trait;

#[async_trait]
pub trait SurfaceService: Send + Sync {
    fn id(&self) -> ServiceId;

    fn name(&self) -> &'static str;

    async fn start(&mut self) -> ServiceResult<()>;

    async fn stop(&mut self) -> ServiceResult<()>;

    async fn tick(&mut self) -> ServiceResult<()>;

    fn status(&self) -> ServiceStatus;
}
Device service
src/services/device.rs
use crate::{
    error::ServiceResult,
    events::{DeviceEvent, EventJournal},
    services::SurfaceService,
    types::{
        DeviceIdentity,
        DeviceSnapshot,
        HealthState,
        ServiceId,
        ServiceState,
        ServiceStatus,
    },
};
use async_trait::async_trait;
use std::sync::Arc;
use std::time::SystemTime;
use uuid::Uuid;

pub struct DeviceService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    journal: Arc<EventJournal>,
    identity: DeviceIdentity,
    restart_count: u32,
    last_heartbeat: Option<SystemTime>,
}

impl DeviceService {
    pub fn new(journal: Arc<EventJournal>) -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            journal,
            identity: DeviceIdentity {
                device_id: Uuid::new_v4(),
                model: "Surface-Generic".into(),
                firmware_version: "unknown".into(),
                os_build: "unknown".into(),
            },
            restart_count: 0,
            last_heartbeat: None,
        }
    }

    pub fn snapshot(&self) -> DeviceSnapshot {
        DeviceSnapshot {
            identity: self.identity.clone(),
            battery_percent: 100.0,
            temperature_celsius: 35.0,
            on_ac_power: true,
            network_connected: true,
        }
    }
}

#[async_trait]
impl SurfaceService for DeviceService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfaceDeviceService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Starting;

        self.journal
            .append(DeviceEvent::info(
                self.name(),
                "Device service starting",
            ))
            .await;

        self.state = ServiceState::Running;
        self.health = HealthState::Healthy;

        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopping;
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());
        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: self.restart_count,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Power service
src/services/power.rs
use crate::{
    error::ServiceResult,
    services::SurfaceService,
    types::{
        HealthState,
        ServiceId,
        ServiceState,
        ServiceStatus,
    },
};
use async_trait::async_trait;
use std::time::SystemTime;
use uuid::Uuid;

pub struct PowerService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    last_heartbeat: Option<SystemTime>,
}

impl PowerService {
    pub fn new() -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            last_heartbeat: None,
        }
    }
}

#[async_trait]
impl SurfaceService for PowerService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfacePowerService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Running;
        self.health = HealthState::Healthy;
        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());

        // Production:
        // Windows power notifications
        // AC/DC state
        // battery status
        // thermal/power policy interaction

        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: 0,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Thermal service
src/services/thermal.rs
use crate::{
    error::ServiceResult,
    services::SurfaceService,
    types::{HealthState, ServiceId, ServiceState, ServiceStatus},
};
use async_trait::async_trait;
use std::time::SystemTime;
use uuid::Uuid;

pub struct ThermalService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    temperature_c: f32,
    last_heartbeat: Option<SystemTime>,
}

impl ThermalService {
    pub fn new() -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            temperature_c: 35.0,
            last_heartbeat: None,
        }
    }

    fn update_health(&mut self) {
        self.health = if self.temperature_c >= 95.0 {
            HealthState::Unhealthy
        } else if self.temperature_c >= 85.0 {
            HealthState::Degraded
        } else {
            HealthState::Healthy
        };
    }
}

#[async_trait]
impl SurfaceService for ThermalService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfaceThermalService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Running;
        self.update_health();
        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());

        // Replace with real Windows/platform thermal telemetry.

        self.update_health();

        if self.health == HealthState::Unhealthy {
            self.state = ServiceState::Degraded;
        }

        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: 0,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Radio service
src/services/radio.rs
use crate::{
    error::ServiceResult,
    services::SurfaceService,
    types::{HealthState, ServiceId, ServiceState, ServiceStatus},
};
use async_trait::async_trait;
use std::time::SystemTime;
use uuid::Uuid;

pub struct RadioService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    wifi_connected: bool,
    bluetooth_enabled: bool,
    last_heartbeat: Option<SystemTime>,
}

impl RadioService {
    pub fn new() -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            wifi_connected: false,
            bluetooth_enabled: false,
            last_heartbeat: None,
        }
    }
}

#[async_trait]
impl SurfaceService for RadioService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfaceRadioService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Running;
        self.health = HealthState::Healthy;
        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());

        // Production integration belongs here:
        //
        // WLAN telemetry
        // Bluetooth device state
        // roaming state
        // connection quality
        //
        // The Rust radio intelligence layer from #17 can
        // provide the policy decision.

        self.health = HealthState::Healthy;

        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: 0,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Display service
src/services/display.rs
use crate::{
    error::ServiceResult,
    services::SurfaceService,
    types::{HealthState, ServiceId, ServiceState, ServiceStatus},
};
use async_trait::async_trait;
use std::time::SystemTime;
use uuid::Uuid;

pub struct DisplayService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    last_heartbeat: Option<SystemTime>,
}

impl DisplayService {
    pub fn new() -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            last_heartbeat: None,
        }
    }
}

#[async_trait]
impl SurfaceService for DisplayService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfaceDisplayService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Running;
        self.health = HealthState::Healthy;
        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());
        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: 0,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Health service
src/services/health.rs
use crate::{
    error::ServiceResult,
    services::SurfaceService,
    types::{HealthState, ServiceId, ServiceState, ServiceStatus},
};
use async_trait::async_trait;
use std::time::SystemTime;
use uuid::Uuid;

pub struct HealthService {
    id: ServiceId,
    state: ServiceState,
    health: HealthState,
    last_heartbeat: Option<SystemTime>,
}

impl HealthService {
    pub fn new() -> Self {
        Self {
            id: Uuid::new_v4(),
            state: ServiceState::Created,
            health: HealthState::Unknown,
            last_heartbeat: None,
        }
    }
}

#[async_trait]
impl SurfaceService for HealthService {
    fn id(&self) -> ServiceId {
        self.id
    }

    fn name(&self) -> &'static str {
        "SurfaceHealthService"
    }

    async fn start(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Running;
        self.health = HealthState::Healthy;
        Ok(())
    }

    async fn stop(&mut self) -> ServiceResult<()> {
        self.state = ServiceState::Stopped;
        Ok(())
    }

    async fn tick(&mut self) -> ServiceResult<()> {
        self.last_heartbeat = Some(SystemTime::now());
        Ok(())
    }

    fn status(&self) -> ServiceStatus {
        ServiceStatus {
            id: self.id,
            name: self.name().into(),
            state: self.state,
            health: self.health,
            restart_count: 0,
            last_heartbeat: self.last_heartbeat,
            last_error: None,
        }
    }
}
Runtime supervisor

This is the most important part of #21.

src/runtime/supervisor.rs
use crate::{
    error::ServiceResult,
    events::{DeviceEvent, EventJournal},
    services::SurfaceService,
    state::RuntimeState,
};
use std::sync::Arc;
use tokio::time::{interval, Duration};

pub struct ServiceSupervisor {
    services: Vec<Box<dyn SurfaceService>>,
    state: RuntimeState,
    journal: Arc<EventJournal>,
}

impl ServiceSupervisor {
    pub fn new(
        services: Vec<Box<dyn SurfaceService>>,
        state: RuntimeState,
        journal: Arc<EventJournal>,
    ) -> Self {
        Self {
            services,
            state,
            journal,
        }
    }

    pub async fn start(&mut self) -> ServiceResult<()> {
        for service in &mut self.services {
            match service.start().await {
                Ok(()) => {
                    self.journal
                        .append(DeviceEvent::info(
                            "Supervisor",
                            format!("Started {}", service.name()),
                        ))
                        .await;
                }

                Err(error) => {
                    self.journal
                        .append(DeviceEvent::info(
                            "Supervisor",
                            format!(
                                "Failed to start {}: {}",
                                service.name(),
                                error
                            ),
                        ))
                        .await;
                }
            }

            self.state.update_service(service.status()).await;
        }

        Ok(())
    }

    pub async fn run(&mut self, period: Duration) -> ServiceResult<()> {
        let mut ticker = interval(period);

        loop {
            ticker.tick().await;

            for service in &mut self.services {
                if let Err(error) = service.tick().await {
                    self.journal
                        .append(DeviceEvent::info(
                            "Supervisor",
                            format!(
                                "{} tick failed: {}",
                                service.name(),
                                error
                            ),
                        ))
                        .await;
                }

                self.state.update_service(service.status()).await;
            }
        }
    }

    pub async fn stop(&mut self) -> ServiceResult<()> {
        for service in self.services.iter_mut().rev() {
            service.stop().await?;
            self.state.update_service(service.status()).await;
        }

        Ok(())
    }
}
Shutdown handling
src/runtime/shutdown.rs
use tokio::signal;

pub async fn wait_for_shutdown() -> anyhow::Result<()> {
    #[cfg(unix)]
    {
        let mut terminate =
            signal::unix::signal(signal::unix::SignalKind::terminate())?;

        tokio::select! {
            _ = signal::ctrl_c() => {}
            _ = terminate.recv() => {}
        }

        return Ok(());
    }

    #[cfg(windows)]
    {
        signal::ctrl_c().await?;
        Ok(())
    }
}
src/runtime/mod.rs
pub mod shutdown;
pub mod supervisor;
pub mod worker;
Worker supervision
src/runtime/worker.rs
use crate::error::ServiceResult;
use std::future::Future;
use std::time::Duration;
use tokio::time::sleep;

pub async fn run_with_restart<F, Fut>(
    mut worker: F,
    restart_delay: Duration,
) -> ServiceResult<()>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = ServiceResult<()>>,
{
    loop {
        match worker().await {
            Ok(()) => return Ok(()),

            Err(error) => {
                tracing::error!("Worker failed: {}", error);

                sleep(restart_delay).await;
            }
        }
    }
}

This provides a basic restart-on-failure primitive.

For production, restart policy should additionally use:

exponential backoff
maximum restart rate
circuit breaker
failure classification
persistent crash counters
Windows Service Control Manager recovery policy
Health monitor
src/health/monitor.rs
use crate::{
    state::RuntimeState,
    types::{HealthReport, HealthState},
};
use std::time::Instant;

pub struct HealthMonitor {
    started: Instant,
}

impl HealthMonitor {
    pub fn new() -> Self {
        Self {
            started: Instant::now(),
        }
    }

    pub async fn report(&self, state: &RuntimeState) -> HealthReport {
        let services = state.services().await;

        let failed_services = services
            .iter()
            .filter(|service| {
                matches!(
                    service.health,
                    HealthState::Unhealthy
                )
            })
            .count();

        let active_services = services
            .iter()
            .filter(|service| {
                matches!(
                    service.state,
                    crate::types::ServiceState::Running
                        | crate::types::ServiceState::Degraded
                )
            })
            .count();

        let overall_state = if failed_services > 0 {
            HealthState::Degraded
        } else {
            HealthState::Healthy
        };

        HealthReport {
            state: overall_state,
            uptime_seconds: self.started.elapsed().as_secs(),
            active_services,
            failed_services,
        }
    }
}
Watchdog
src/health/watchdog.rs
use crate::{
    state::RuntimeState,
    types::ServiceState,
};
use std::time::{Duration, SystemTime};

pub struct Watchdog {
    timeout: Duration,
}

impl Watchdog {
    pub fn new(timeout: Duration) -> Self {
        Self { timeout }
    }

    pub async fn unhealthy_services(
        &self,
        state: &RuntimeState,
    ) -> Vec<String> {
        let now = SystemTime::now();
        let services = state.services().await;

        services
            .into_iter()
            .filter_map(|service| {
                let heartbeat = service.last_heartbeat?;

                let age = now
                    .duration_since(heartbeat)
                    .unwrap_or_default();

                if age > self.timeout
                    || service.state == ServiceState::Failed
                {
                    Some(service.name)
                } else {
                    None
                }
            })
            .collect()
    }
}
src/health/mod.rs
pub mod monitor;
pub mod watchdog;

pub use monitor::HealthMonitor;
pub use watchdog::Watchdog;
IPC protocol

The daemon should have a small, versioned control protocol rather than allowing arbitrary internal state manipulation.

src/ipc/protocol.rs
use crate::types::{HealthReport, ServiceStatus};
use serde::{Deserialize, Serialize};

pub const IPC_VERSION: u16 = 1;

#[derive(Debug, Serialize, Deserialize)]
pub struct Request {
    pub version: u16,
    pub command: Command,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum Command {
    GetHealth,
    GetServices,
    GetDevice,
    GetEvents {
        count: usize,
    },
    Ping,
}

#[derive(Debug, Serialize, Deserialize)]
pub enum Response {
    Pong,

    Health(HealthReport),

    Services(Vec<ServiceStatus>),

    Device(serde_json::Value),

    Events(Vec<crate::events::DeviceEvent>),

    Error {
        message: String,
    },
}
src/ipc/mod.rs
pub mod protocol;
Windows integration boundary

The critical design decision is not pretending that private Surface hardware APIs are public APIs.

src/windows/adapters.rs
use crate::{
    error::{ServiceError, ServiceResult},
    types::DeviceSnapshot,
};

pub trait DeviceAdapter: Send + Sync {
    fn read_snapshot(&self) -> ServiceResult<DeviceSnapshot>;

    fn initialize(&self) -> ServiceResult<()>;

    fn shutdown(&self) -> ServiceResult<()>;
}

pub struct WindowsDeviceAdapter;

impl WindowsDeviceAdapter {
    pub fn new() -> Self {
        Self
    }
}

impl DeviceAdapter for WindowsDeviceAdapter {
    fn read_snapshot(&self) -> ServiceResult<DeviceSnapshot> {
        Err(ServiceError::DeviceAdapter(
            "Windows Surface telemetry adapter not connected"
                .into(),
        ))
    }

    fn initialize(&self) -> ServiceResult<()> {
        Ok(())
    }

    fn shutdown(&self) -> ServiceResult<()> {
        Ok(())
    }
}

This is deliberate.

The production adapter can subsequently connect to appropriate supported Windows mechanisms for:

Power
 ├── battery
 ├── AC state
 └── power notifications

Thermal
 ├── supported sensor interfaces
 └── platform telemetry

PnP
 ├── device arrival
 ├── device removal
 └── device state

Networking
 ├── WLAN
 └── Bluetooth

Display
 ├── monitor topology
 └── display state

Audio
 └── Windows audio APIs

Pen/Touch
 └── Windows pointer/input stack

Security
 ├── Windows security APIs
 ├── CNG
 └── TPM-backed facilities
Windows service wrapper
src/windows/service.rs
#[cfg(windows)]
pub struct WindowsServiceHost {
    service_name: String,
}

#[cfg(windows)]
impl WindowsServiceHost {
    pub fn new(name: impl Into<String>) -> Self {
        Self {
            service_name: name.into(),
        }
    }

    pub fn run(&self) -> anyhow::Result<()> {
        tracing::info!(
            "Starting Windows service: {}",
            self.service_name
        );

        // Production implementation:
        //
        // StartServiceCtrlDispatcherW
        // RegisterServiceCtrlHandlerExW
        // SERVICE_STATUS
        // SERVICE_STATUS_HANDLE
        //
        // The async Tokio runtime is hosted underneath
        // the Windows Service Control Manager callback layer.

        Ok(())
    }
}
src/windows/mod.rs
pub mod adapters;
pub mod service;
Main service
src/main.rs
use std::sync::Arc;

use surface_device_services::{
    config::Config,
    events::EventJournal,
    health::{HealthMonitor, Watchdog},
    runtime::shutdown::wait_for_shutdown,
    runtime::supervisor::ServiceSupervisor,
    services::{
        device::DeviceService,
        display::DisplayService,
        health::HealthService,
        power::PowerService,
        radio::RadioService,
        thermal::ThermalService,
        SurfaceService,
    },
    state::RuntimeState,
};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            std::env::var("RUST_LOG")
                .unwrap_or_else(|_| "info".into()),
        )
        .init();

    tracing::info!("Surface Device Services starting");

    let config = Config::default();

    let state = RuntimeState::new();

    let journal = Arc::new(
        EventJournal::new(config.events.capacity),
    );

    let services: Vec<Box<dyn SurfaceService>> = vec![
        Box::new(DeviceService::new(journal.clone())),
        Box::new(PowerService::new()),
        Box::new(ThermalService::new()),
        Box::new(RadioService::new()),
        Box::new(DisplayService::new()),
        Box::new(HealthService::new()),
    ];

    let mut supervisor =
        ServiceSupervisor::new(
            services,
            state.clone(),
            journal.clone(),
        );

    supervisor.start().await?;

    let monitor = HealthMonitor::new();

    let watchdog =
        Watchdog::new(config.watchdog_timeout());

    let run_state = state.clone();

    let heartbeat =
        config.heartbeat_interval();

    let supervisor_task =
        tokio::spawn(async move {
            supervisor.run(heartbeat).await
        });

    tokio::select! {
        result = supervisor_task => {
            tracing::error!(
                "Supervisor exited: {:?}",
                result
            );
        }

        result = wait_for_shutdown() => {
            result?;
            tracing::info!(
                "Shutdown requested"
            );
        }
    }

    let report =
        monitor.report(&run_state).await;

    tracing::info!(
        "Final health: {:?}, uptime={}s, active={}, failed={}",
        report.state,
        report.uptime_seconds,
        report.active_services,
        report.failed_services
    );

    let unhealthy =
        watchdog.unhealthy_services(&run_state).await;

    for service in unhealthy {
        tracing::warn!(
            "Watchdog detected unhealthy service: {}",
            service
        );
    }

    tracing::info!(
        "Surface Device Services stopped"
    );

    Ok(())
}
Tests
tests/health_tests.rs
use surface_device_services::{
    health::HealthMonitor,
    state::RuntimeState,
    types::HealthState,
};

#[tokio::test]
async fn empty_runtime_is_healthy() {
    let state = RuntimeState::new();

    let monitor = HealthMonitor::new();

    let report = monitor.report(&state).await;

    assert_eq!(
        report.state,
        HealthState::Healthy
    );

    assert_eq!(
        report.active_services,
        0
    );
}
tests/event_tests.rs
use surface_device_services::events::{
    DeviceEvent,
    EventJournal,
};

#[tokio::test]
async fn journal_respects_capacity() {
    let journal = EventJournal::new(2);

    journal
        .append(DeviceEvent::info("test", "one"))
        .await;

    journal
        .append(DeviceEvent::info("test", "two"))
        .await;

    journal
        .append(DeviceEvent::info("test", "three"))
        .await;

    assert_eq!(journal.len().await, 2);

    let events =
        journal.recent(10).await;

    assert_eq!(events.len(), 2);
}
tests/runtime_tests.rs
use std::sync::Arc;

use surface_device_services::{
    events::EventJournal,
    services::{
        device::DeviceService,
        SurfaceService,
    },
};

#[tokio::test]
async fn device_service_starts() {
    let journal =
        Arc::new(EventJournal::new(32));

    let mut service =
        DeviceService::new(journal);

    service.start().await.unwrap();

    let status =
        service.status();

    assert_eq!(
        status.state,
        surface_device_services::types::ServiceState::Running
    );

    assert_eq!(
        status.health,
        surface_device_services::types::HealthState::Healthy
    );
}
config/default.toml
[runtime]
heartbeat_seconds = 5
shutdown_timeout_seconds = 10

[health]
interval_seconds = 5
watchdog_timeout_seconds = 15

[events]
capacity = 4096






Project structure
SurfaceDiagnostics/
├── pyproject.toml
├── README.md
│
├── surface_diag/
│   ├── __init__.py
│   ├── cli.py
│   ├── config.py
│   │
│   ├── models/
│   │   ├── __init__.py
│   │   ├── telemetry.py
│   │   ├── events.py
│   │   └── report.py
│   │
│   ├── capture/
│   │   ├── __init__.py
│   │   ├── collector.py
│   │   ├── file_reader.py
│   │   └── simulator.py
│   │
│   ├── analysis/
│   │   ├── __init__.py
│   │   ├── battery.py
│   │   ├── thermal.py
│   │   ├── performance.py
│   │   ├── radio.py
│   │   ├── display.py
│   │   └── anomalies.py
│   │
│   ├── benchmark/
│   │   ├── __init__.py
│   │   ├── runner.py
│   │   └── statistics.py
│   │
│   └── reporting/
│       ├── __init__.py
│       ├── html.py
│       ├── json_report.py
│       └── summary.py
│
├── tools/
│   └── run_diagnostics.py
│
└── tests/
    ├── test_battery.py
    ├── test_thermal.py
    ├── test_anomalies.py
    └── test_benchmark.py
pyproject.toml
[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[project]
name = "surface-diagnostics"
version = "0.1.0"
description = "Python diagnostics and engineering toolkit for Surface devices"
requires-python = ">=3.11"

dependencies = [
    "numpy>=2.0",
    "pandas>=2.2",
    "scipy>=1.13"
]

[project.optional-dependencies]
visualization = [
    "matplotlib>=3.9"
]

development = [
    "pytest>=8",
    "pytest-cov>=5"
]

[project.scripts]
surface-diag = "surface_diag.cli:main"
Telemetry model
surface_diag/models/telemetry.py
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Optional


@dataclass(frozen=True)
class TelemetrySample:
    timestamp: datetime

    battery_percent: float
    battery_power_watts: float
    charging: bool

    cpu_percent: float
    gpu_percent: float
    npu_percent: float

    cpu_temperature_c: float
    gpu_temperature_c: float

    cpu_frequency_mhz: float
    gpu_frequency_mhz: float

    memory_used_percent: float

    wifi_rssi_dbm: Optional[float]
    wifi_latency_ms: Optional[float]
    wifi_packet_loss_percent: Optional[float]

    display_refresh_hz: float
    display_brightness_percent: float

    fan_rpm: Optional[float] = None


@dataclass
class TelemetryDataset:
    samples: list[TelemetrySample]

    @property
    def count(self) -> int:
        return len(self.samples)

    def timestamps(self):
        return [sample.timestamp for sample in self.samples]
Event model
surface_diag/models/events.py
from dataclasses import dataclass
from datetime import datetime
from enum import Enum


class Severity(str, Enum):
    DEBUG = "debug"
    INFO = "info"
    WARNING = "warning"
    ERROR = "error"
    CRITICAL = "critical"


@dataclass(frozen=True)
class DiagnosticEvent:
    timestamp: datetime
    source: str
    severity: Severity
    message: str
    event_id: str | None = None
Report model
surface_diag/models/report.py
from dataclasses import dataclass, field
from typing import Any


@dataclass
class DiagnosticFinding:
    subsystem: str
    severity: str
    title: str
    description: str
    metrics: dict[str, Any] = field(default_factory=dict)


@dataclass
class DiagnosticReport:
    device_id: str
    sample_count: int

    findings: list[DiagnosticFinding] = field(
        default_factory=list
    )

    metrics: dict[str, Any] = field(
        default_factory=dict
    )

    def add_finding(
        self,
        subsystem: str,
        severity: str,
        title: str,
        description: str,
        **metrics: Any,
    ) -> None:
        self.findings.append(
            DiagnosticFinding(
                subsystem=subsystem,
                severity=severity,
                title=title,
                description=description,
                metrics=metrics,
            )
        )
Capture layer
surface_diag/capture/file_reader.py
from __future__ import annotations

import json
from datetime import datetime

from surface_diag.models.telemetry import (
    TelemetryDataset,
    TelemetrySample,
)


def load_json(path: str) -> TelemetryDataset:
    with open(path, "r", encoding="utf-8") as file:
        data = json.load(file)

    samples = []

    for item in data:
        samples.append(
            TelemetrySample(
                timestamp=datetime.fromisoformat(
                    item["timestamp"]
                ),
                battery_percent=float(
                    item["battery_percent"]
                ),
                battery_power_watts=float(
                    item["battery_power_watts"]
                ),
                charging=bool(item["charging"]),
                cpu_percent=float(item["cpu_percent"]),
                gpu_percent=float(item["gpu_percent"]),
                npu_percent=float(item["npu_percent"]),
                cpu_temperature_c=float(
                    item["cpu_temperature_c"]
                ),
                gpu_temperature_c=float(
                    item["gpu_temperature_c"]
                ),
                cpu_frequency_mhz=float(
                    item["cpu_frequency_mhz"]
                ),
                gpu_frequency_mhz=float(
                    item["gpu_frequency_mhz"]
                ),
                memory_used_percent=float(
                    item["memory_used_percent"]
                ),
                wifi_rssi_dbm=item.get("wifi_rssi_dbm"),
                wifi_latency_ms=item.get("wifi_latency_ms"),
                wifi_packet_loss_percent=item.get(
                    "wifi_packet_loss_percent"
                ),
                display_refresh_hz=float(
                    item["display_refresh_hz"]
                ),
                display_brightness_percent=float(
                    item["display_brightness_percent"]
                ),
                fan_rpm=item.get("fan_rpm"),
            )
        )

    return TelemetryDataset(samples)
Diagnostic simulator

This makes the framework testable without pretending to have access to private Surface hardware.

surface_diag/capture/simulator.py
from __future__ import annotations

import math
import random
from datetime import datetime, timedelta

from surface_diag.models.telemetry import (
    TelemetryDataset,
    TelemetrySample,
)


def generate_samples(
    count: int = 300,
    interval_seconds: float = 1.0,
    seed: int = 42,
) -> TelemetryDataset:
    rng = random.Random(seed)

    start = datetime.now()

    samples = []

    for i in range(count):
        t = i * interval_seconds

        cpu = (
            20
            + 15 * math.sin(t / 20)
            + rng.uniform(-3, 3)
        )

        gpu = (
            10
            + 20 * math.sin(t / 30)
            + rng.uniform(-3, 3)
        )

        cpu = max(0, min(100, cpu))
        gpu = max(0, min(100, gpu))

        cpu_temp = (
            42
            + cpu * 0.42
            + rng.uniform(-1.5, 1.5)
        )

        battery = max(
            5,
            80 - (i / count) * 12
        )

        samples.append(
            TelemetrySample(
                timestamp=start + timedelta(
                    seconds=t
                ),
                battery_percent=battery,
                battery_power_watts=(
                    8 + cpu * 0.12
                ),
                charging=False,
                cpu_percent=cpu,
                gpu_percent=gpu,
                npu_percent=0,
                cpu_temperature_c=cpu_temp,
                gpu_temperature_c=48 + gpu * 0.35,
                cpu_frequency_mhz=1800 + cpu * 18,
                gpu_frequency_mhz=500 + gpu * 10,
                memory_used_percent=45 + cpu * 0.2,
                wifi_rssi_dbm=-48,
                wifi_latency_ms=12,
                wifi_packet_loss_percent=0.2,
                display_refresh_hz=120,
                display_brightness_percent=65,
                fan_rpm=1800 + cpu * 15,
            )
        )

    return TelemetryDataset(samples)
Battery analysis
surface_diag/analysis/battery.py
from __future__ import annotations

import numpy as np

from surface_diag.models.telemetry import TelemetryDataset


def analyze_battery(
    dataset: TelemetryDataset,
) -> dict[str, float]:
    if not dataset.samples:
        return {}

    battery = np.array([
        sample.battery_percent
        for sample in dataset.samples
    ])

    power = np.array([
        sample.battery_power_watts
        for sample in dataset.samples
        if not sample.charging
    ])

    result = {
        "initial_percent": float(battery[0]),
        "final_percent": float(battery[-1]),
        "minimum_percent": float(np.min(battery)),
        "maximum_percent": float(np.max(battery)),
    }

    if len(power):
        result["average_power_watts"] = float(
            np.mean(power)
        )

        result["peak_power_watts"] = float(
            np.max(power)
        )

    return result
Thermal analysis
surface_diag/analysis/thermal.py
from __future__ import annotations

import numpy as np

from surface_diag.models.telemetry import TelemetryDataset


def analyze_thermal(
    dataset: TelemetryDataset,
) -> dict[str, float | int]:
    if not dataset.samples:
        return {}

    cpu = np.array([
        s.cpu_temperature_c
        for s in dataset.samples
    ])

    gpu = np.array([
        s.gpu_temperature_c
        for s in dataset.samples
    ])

    result = {
        "cpu_average_c": float(np.mean(cpu)),
        "cpu_peak_c": float(np.max(cpu)),
        "gpu_average_c": float(np.mean(gpu)),
        "gpu_peak_c": float(np.max(gpu)),
        "cpu_over_85_count": int(
            np.sum(cpu >= 85)
        ),
        "cpu_over_95_count": int(
            np.sum(cpu >= 95)
        ),
    }

    return result
Performance analysis
surface_diag/analysis/performance.py
from __future__ import annotations

import numpy as np

from surface_diag.models.telemetry import TelemetryDataset


def analyze_performance(
    dataset: TelemetryDataset,
) -> dict[str, float]:
    cpu = np.array([
        s.cpu_percent
        for s in dataset.samples
    ])

    gpu = np.array([
        s.gpu_percent
        for s in dataset.samples
    ])

    memory = np.array([
        s.memory_used_percent
        for s in dataset.samples
    ])

    return {
        "cpu_average_percent": float(
            np.mean(cpu)
        ),
        "cpu_peak_percent": float(
            np.max(cpu)
        ),
        "gpu_average_percent": float(
            np.mean(gpu)
        ),
        "gpu_peak_percent": float(
            np.max(gpu)
        ),
        "memory_average_percent": float(
            np.mean(memory)
        ),
        "memory_peak_percent": float(
            np.max(memory)
        ),
    }
Radio analysis
surface_diag/analysis/radio.py
from __future__ import annotations

import numpy as np

from surface_diag.models.telemetry import TelemetryDataset


def analyze_radio(
    dataset: TelemetryDataset,
) -> dict[str, float]:
    rssi = [
        s.wifi_rssi_dbm
        for s in dataset.samples
        if s.wifi_rssi_dbm is not None
    ]

    latency = [
        s.wifi_latency_ms
        for s in dataset.samples
        if s.wifi_latency_ms is not None
    ]

    packet_loss = [
        s.wifi_packet_loss_percent
        for s in dataset.samples
        if s.wifi_packet_loss_percent is not None
    ]

    result = {}

    if rssi:
        result["average_rssi_dbm"] = float(
            np.mean(rssi)
        )

        result["minimum_rssi_dbm"] = float(
            np.min(rssi)
        )

    if latency:
        result["average_latency_ms"] = float(
            np.mean(latency)
        )

    if packet_loss:
        result["average_packet_loss_percent"] = float(
            np.mean(packet_loss)
        )

    return result
Display analysis
surface_diag/analysis/display.py
from __future__ import annotations

import numpy as np

from surface_diag.models.telemetry import TelemetryDataset


def analyze_display(
    dataset: TelemetryDataset,
) -> dict[str, float]:
    refresh = np.array([
        s.display_refresh_hz
        for s in dataset.samples
    ])

    brightness = np.array([
        s.display_brightness_percent
        for s in dataset.samples
    ])

    return {
        "average_refresh_hz": float(
            np.mean(refresh)
        ),
        "minimum_refresh_hz": float(
            np.min(refresh)
        ),
        "maximum_refresh_hz": float(
            np.max(refresh)
        ),
        "average_brightness_percent": float(
            np.mean(brightness)
        ),
    }
Automatic anomaly detection

This is where Python becomes particularly useful.

surface_diag/analysis/anomalies.py
from __future__ import annotations

import numpy as np

from surface_diag.models.report import (
    DiagnosticReport,
)
from surface_diag.models.telemetry import (
    TelemetryDataset,
)


def detect_anomalies(
    dataset: TelemetryDataset,
    report: DiagnosticReport,
) -> None:
    if not dataset.samples:
        return

    cpu_temp = np.array([
        s.cpu_temperature_c
        for s in dataset.samples
    ])

    cpu_load = np.array([
        s.cpu_percent
        for s in dataset.samples
    ])

    battery_power = np.array([
        s.battery_power_watts
        for s in dataset.samples
        if not s.charging
    ])

    if np.max(cpu_temp) >= 95:
        report.add_finding(
            "thermal",
            "critical",
            "High CPU temperature",
            "CPU temperature reached or exceeded 95 C.",
            peak_temperature_c=float(
                np.max(cpu_temp)
            ),
        )

    elif np.max(cpu_temp) >= 85:
        report.add_finding(
            "thermal",
            "warning",
            "Elevated CPU temperature",
            "CPU temperature repeatedly entered the high-temperature range.",
            peak_temperature_c=float(
                np.max(cpu_temp)
            ),
        )

    if len(cpu_load) > 10:
        high_load = cpu_load >= 90

        if np.mean(high_load) > 0.30:
            report.add_finding(
                "performance",
                "warning",
                "Sustained CPU load",
                "CPU utilization remained above 90% for a substantial portion of the capture.",
                high_load_fraction=float(
                    np.mean(high_load)
                ),
            )

    if len(battery_power):
        p95 = float(
            np.percentile(battery_power, 95)
        )

        if p95 > 35:
            report.add_finding(
                "power",
                "warning",
                "High battery power draw",
                "Observed battery power exceeded the diagnostic threshold.",
                p95_power_watts=p95,
            )
Combined analysis
surface_diag/analysis/__init__.py
from surface_diag.analysis.battery import analyze_battery
from surface_diag.analysis.thermal import analyze_thermal
from surface_diag.analysis.performance import (
    analyze_performance,
)
from surface_diag.analysis.radio import analyze_radio
from surface_diag.analysis.display import analyze_display
from surface_diag.analysis.anomalies import detect_anomalies

__all__ = [
    "analyze_battery",
    "analyze_thermal",
    "analyze_performance",
    "analyze_radio",
    "analyze_display",
    "detect_anomalies",
]
Benchmarking
surface_diag/benchmark/runner.py
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable


@dataclass
class BenchmarkResult:
    name: str
    iterations: int
    total_seconds: float

    @property
    def average_seconds(self) -> float:
        return self.total_seconds / self.iterations

    @property
    def operations_per_second(self) -> float:
        return self.iterations / self.total_seconds


def benchmark(
    name: str,
    function: Callable[[], object],
    iterations: int = 100,
) -> BenchmarkResult:
    start = time.perf_counter()

    for _ in range(iterations):
        function()

    elapsed = time.perf_counter() - start

    return BenchmarkResult(
        name=name,
        iterations=iterations,
        total_seconds=elapsed,
    )
Statistical analysis
surface_diag/benchmark/statistics.py
from __future__ import annotations

import numpy as np


def percentile_summary(
    values: list[float],
) -> dict[str, float]:
    array = np.asarray(values)

    return {
        "minimum": float(np.min(array)),
        "p50": float(np.percentile(array, 50)),
        "p90": float(np.percentile(array, 90)),
        "p95": float(np.percentile(array, 95)),
        "p99": float(np.percentile(array, 99)),
        "maximum": float(np.max(array)),
        "mean": float(np.mean(array)),
        "stddev": float(np.std(array)),
    }
HTML engineering report
surface_diag/reporting/html.py
from __future__ import annotations

import html

from surface_diag.models.report import (
    DiagnosticReport,
)


def generate_html(
    report: DiagnosticReport,
) -> str:
    findings = []

    for finding in report.findings:
        metrics = "".join(
            f"<li><b>{html.escape(str(k))}</b>: "
            f"{html.escape(str(v))}</li>"
            for k, v in finding.metrics.items()
        )

        findings.append(
            f"""
            <section>
                <h2>{html.escape(finding.title)}</h2>
                <p>
                    <b>Subsystem:</b>
                    {html.escape(finding.subsystem)}
                </p>
                <p>
                    <b>Severity:</b>
                    {html.escape(finding.severity)}
                </p>
                <p>
                    {html.escape(finding.description)}
                </p>
                <ul>{metrics}</ul>
            </section>
            """
        )

    metrics_html = "".join(
        f"<tr><td>{html.escape(str(k))}</td>"
        f"<td>{html.escape(str(v))}</td></tr>"
        for k, v in report.metrics.items()
    )

    return f"""
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Surface Diagnostic Report</title>

<style>
body {{
    font-family: system-ui, sans-serif;
    max-width: 1100px;
    margin: 40px auto;
    padding: 0 24px;
}}

header {{
    border-bottom: 1px solid #ccc;
    margin-bottom: 30px;
}}

section {{
    border: 1px solid #ddd;
    border-radius: 8px;
    padding: 18px;
    margin: 18px 0;
}}

table {{
    border-collapse: collapse;
    width: 100%;
}}

td {{
    border-bottom: 1px solid #ddd;
    padding: 8px;
}}
</style>
</head>

<body>

<header>
<h1>Surface Diagnostic Report</h1>
<p>Device: {html.escape(report.device_id)}</p>
<p>Samples: {report.sample_count}</p>
</header>

<h2>Metrics</h2>

<table>
{metrics_html}
</table>

<h2>Findings</h2>

{''.join(findings)}

</body>
</html>
"""
JSON report
surface_diag/reporting/json_report.py
from __future__ import annotations

import json
from dataclasses import asdict

from surface_diag.models.report import DiagnosticReport


def write_json(
    report: DiagnosticReport,
    path: str,
) -> None:
    with open(
        path,
        "w",
        encoding="utf-8",
    ) as file:
        json.dump(
            asdict(report),
            file,
            indent=2,
            default=str,
        )
CLI
surface_diag/cli.py
from __future__ import annotations

import argparse

from surface_diag.analysis import (
    analyze_battery,
    analyze_display,
    analyze_performance,
    analyze_radio,
    analyze_thermal,
    detect_anomalies,
)
from surface_diag.capture.file_reader import load_json
from surface_diag.capture.simulator import generate_samples
from surface_diag.models.report import DiagnosticReport
from surface_diag.reporting.html import generate_html


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="surface-diag"
    )

    parser.add_argument(
        "--input",
        help="Telemetry JSON file",
    )

    parser.add_argument(
        "--output",
        default="surface-report.html",
    )

    parser.add_argument(
        "--simulate",
        action="store_true",
    )

    args = parser.parse_args()

    if args.input:
        dataset = load_json(args.input)
    else:
        dataset = generate_samples()

    report = DiagnosticReport(
        device_id="Surface-Engineering-Unit",
        sample_count=dataset.count,
    )

    report.metrics.update(
        {
            "battery": analyze_battery(dataset),
            "thermal": analyze_thermal(dataset),
            "performance": analyze_performance(dataset),
            "radio": analyze_radio(dataset),
            "display": analyze_display(dataset),
        }
    )

    detect_anomalies(
        dataset,
        report,
    )

    html_report = generate_html(report)

    with open(
        args.output,
        "w",
        encoding="utf-8",
    ) as file:
        file.write(html_report)

    print(
        f"Diagnostic report written to {args.output}"
    )

    print(
        f"Samples analysed: {dataset.count}"
    )

    print(
        f"Findings: {len(report.findings)}"
    )


if __name__ == "__main__":
    main()
Tests
tests/test_battery.py
from datetime import datetime

from surface_diag.analysis.battery import (
    analyze_battery,
)
from surface_diag.models.telemetry import (
    TelemetryDataset,
    TelemetrySample,
)


def make_sample(battery: float):
    return TelemetrySample(
        timestamp=datetime.now(),
        battery_percent=battery,
        battery_power_watts=10,
        charging=False,
        cpu_percent=30,
        gpu_percent=20,
        npu_percent=0,
        cpu_temperature_c=50,
        gpu_temperature_c=45,
        cpu_frequency_mhz=2000,
        gpu_frequency_mhz=800,
        memory_used_percent=40,
        wifi_rssi_dbm=-50,
        wifi_latency_ms=10,
        wifi_packet_loss_percent=0,
        display_refresh_hz=120,
        display_brightness_percent=50,
    )


def test_battery_analysis():
    dataset = TelemetryDataset(
        [
            make_sample(90),
            make_sample(80),
            make_sample(70),
        ]
    )

    result = analyze_battery(dataset)

    assert result["initial_percent"] == 90
    assert result["final_percent"] == 70
    assert result["minimum_percent"] == 70
tests/test_thermal.py
from datetime import datetime

from surface_diag.analysis.thermal import (
    analyze_thermal,
)
from surface_diag.models.telemetry import (
    TelemetryDataset,
    TelemetrySample,
)


def make_sample(temp: float):
    return TelemetrySample(
        timestamp=datetime.now(),
        battery_percent=80,
        battery_power_watts=10,
        charging=False,
        cpu_percent=50,
        gpu_percent=20,
        npu_percent=0,
        cpu_temperature_c=temp,
        gpu_temperature_c=50,
        cpu_frequency_mhz=2000,
        gpu_frequency_mhz=800,
        memory_used_percent=40,
        wifi_rssi_dbm=-50,
        wifi_latency_ms=10,
        wifi_packet_loss_percent=0,
        display_refresh_hz=120,
        display_brightness_percent=50,
    )


def test_thermal_peak():
    dataset = TelemetryDataset(
        [
            make_sample(50),
            make_sample(90),
            make_sample(70),
        ]
    )

    result = analyze_thermal(dataset)

    assert result["cpu_peak_c"] == 90
    assert result["cpu_over_85_count"] == 1
tests/test_anomalies.py
from datetime import datetime

from surface_diag.analysis.anomalies import (
    detect_anomalies,
)
from surface_diag.models.report import (
    DiagnosticReport,
)
from surface_diag.models.telemetry import (
    TelemetryDataset,
    TelemetrySample,
)


def test_high_temperature_detection():
    sample = TelemetrySample(
        timestamp=datetime.now(),
        battery_percent=50,
        battery_power_watts=20,
        charging=False,
        cpu_percent=50,
        gpu_percent=20,
        npu_percent=0,
        cpu_temperature_c=96,
        gpu_temperature_c=70,
        cpu_frequency_mhz=3000,
        gpu_frequency_mhz=1000,
        memory_used_percent=50,
        wifi_rssi_dbm=-50,
        wifi_latency_ms=10,
        wifi_packet_loss_percent=0,
        display_refresh_hz=120,
        display_brightness_percent=50,
    )

    dataset = TelemetryDataset([sample])

    report = DiagnosticReport(
        device_id="TEST",
        sample_count=1,
    )

    detect_anomalies(
        dataset,
        report,
    )

    assert len(report.findings) == 1
    assert report.findings[0].severity == "critical"
Running it
python -m venv .venv

# Windows
.venv\Scripts\activate

pip install -e ".[development]"

surface-diag --simulate --output surface-report.html

Or against captured telemetry:

surface-diag \
    --input telemetry.json \
    --output engineering-report.html

Tests:

pytest









Project structure
SurfacePerformanceAnalytics/
├── Project.toml
├── README.md
│
├── julia/
│   ├── SurfaceAnalytics.jl
│   ├── Types.jl
│   ├── Loader.jl
│   ├── Statistics.jl
│   ├── ThermalModel.jl
│   ├── PowerModel.jl
│   ├── PerformanceModel.jl
│   ├── BottleneckModel.jl
│   ├── Regression.jl
│   ├── Forecast.jl
│   ├── Optimizer.jl
│   └── AnalysisPipeline.jl
│
├── python/
│   ├── load_data.py
│   ├── visualise.py
│   ├── dashboard.py
│   ├── report.py
│   └── run_analysis.py
│
├── data/
│   └── sample_telemetry.csv
│
└── tests/
    ├── julia_tests.jl
    └── test_python.py
Julia numerical engine
Project.toml
name = "SurfacePerformanceAnalytics"
uuid = "4b2c4e34-1c8d-4a9c-b7e7-0c3f72d9f201"
authors = ["Surface Engineering"]
version = "0.1.0"

[deps]
CSV = "336ed68f-0bac-5ca0-87d4-7b16cafc5d65"
DataFrames = "a93c6f00-e57d-5684-b7b6-d8193f7be0c0"
Statistics = "10745b16-90d9-50b4-8f94-1f3f8f4e0e9d"
LinearAlgebra = "37e2e46d-f3b7-5375-84f5-2f5b7f9e6d3d"
Core data types
julia/Types.jl
module SurfaceTypes

export Telemetry, AnalysisResult, BottleneckResult

struct Telemetry
    timestamp::Float64

    battery_percent::Float64
    battery_power_watts::Float64
    charging::Bool

    cpu_percent::Float64
    gpu_percent::Float64
    npu_percent::Float64

    cpu_temperature_c::Float64
    gpu_temperature_c::Float64

    cpu_frequency_mhz::Float64
    gpu_frequency_mhz::Float64

    memory_used_percent::Float64

    wifi_rssi_dbm::Float64
    wifi_latency_ms::Float64
    wifi_packet_loss_percent::Float64

    display_refresh_hz::Float64
    display_brightness_percent::Float64
end

struct BottleneckResult
    cpu_score::Float64
    gpu_score::Float64
    memory_score::Float64
    thermal_score::Float64
    power_score::Float64
end

struct AnalysisResult
    metrics::Dict{String,Float64}
    bottleneck::BottleneckResult
end

end
CSV loader
julia/Loader.jl
module SurfaceLoader

using CSV
using DataFrames

export load_telemetry

function load_telemetry(path::String)
    return CSV.read(
        path,
        DataFrame;
        normalizenames=true
    )
end

end
Statistical engine
julia/Statistics.jl
module SurfaceStatistics

using Statistics

export summarize

function summarize(values)
    return Dict(
        "mean" => mean(values),
        "minimum" => minimum(values),
        "maximum" => maximum(values),
        "stddev" => std(values),
        "median" => median(values),
        "p95" => quantile(values, 0.95),
        "p99" => quantile(values, 0.99)
    )
end

end

This is the basic statistical primitive that everything else uses.

Thermal model
julia/ThermalModel.jl
module ThermalModel

using Statistics

export thermal_metrics, thermal_pressure

function thermal_metrics(df)
    cpu = Float64.(df.cpu_temperature_c)
    gpu = Float64.(df.gpu_temperature_c)

    return Dict(
        "cpu_mean_c" => mean(cpu),
        "cpu_peak_c" => maximum(cpu),
        "gpu_mean_c" => mean(gpu),
        "gpu_peak_c" => maximum(gpu),
        "cpu_over_85_fraction" =>
            mean(cpu .>= 85.0),
        "cpu_over_95_fraction" =>
            mean(cpu .>= 95.0)
    )
end

function thermal_pressure(
    temperature::Real,
    threshold::Real=85.0,
    critical::Real=100.0
)
    if temperature <= threshold
        return 0.0
    end

    return clamp(
        (temperature - threshold) /
        (critical - threshold),
        0.0,
        1.0
    )
end

end
Power model
julia/PowerModel.jl
module PowerModel

using Statistics

export power_metrics, energy_estimate

function power_metrics(df)
    power = Float64.(df.battery_power_watts)

    return Dict(
        "mean_power_watts" => mean(power),
        "peak_power_watts" => maximum(power),
        "p95_power_watts" =>
            quantile(power, 0.95)
    )
end

function energy_estimate(
    power_watts,
    timestamps
)
    if length(power_watts) < 2
        return 0.0
    end

    energy_wh = 0.0

    for i in 2:length(power_watts)
        dt =
            timestamps[i] -
            timestamps[i - 1]

        energy_wh +=
            0.5 *
            (power_watts[i] + power_watts[i - 1]) *
            dt / 3600.0
    end

    return energy_wh
end

end

This uses trapezoidal integration, rather than simply multiplying average power by an assumed duration.

Performance model
julia/PerformanceModel.jl
module PerformanceModel

using Statistics

export performance_metrics

function performance_metrics(df)
    cpu = Float64.(df.cpu_percent)
    gpu = Float64.(df.gpu_percent)
    npu = Float64.(df.npu_percent)
    memory = Float64.(df.memory_used_percent)

    return Dict(
        "cpu_mean_percent" => mean(cpu),
        "cpu_peak_percent" => maximum(cpu),

        "gpu_mean_percent" => mean(gpu),
        "gpu_peak_percent" => maximum(gpu),

        "npu_mean_percent" => mean(npu),
        "npu_peak_percent" => maximum(npu),

        "memory_mean_percent" =>
            mean(memory),

        "memory_peak_percent" =>
            maximum(memory)
    )
end

end
Bottleneck model

This is where Julia starts doing something more interesting than merely calculating averages.

julia/BottleneckModel.jl
module BottleneckModel

export calculate_bottleneck

function normalize(
    value::Real,
    low::Real,
    high::Real
)
    return clamp(
        (value - low) /
        (high - low),
        0.0,
        1.0
    )
end

function calculate_bottleneck(df)
    cpu = mean(df.cpu_percent) / 100.0
    gpu = mean(df.gpu_percent) / 100.0
    memory = mean(df.memory_used_percent) / 100.0

    thermal = mean(
        normalize.(
            df.cpu_temperature_c,
            60.0,
            100.0
        )
    )

    power = mean(
        normalize.(
            df.battery_power_watts,
            5.0,
            50.0
        )
    )

    return Dict(
        "cpu_score" => cpu,
        "gpu_score" => gpu,
        "memory_score" => memory,
        "thermal_score" => thermal,
        "power_score" => power
    )
end

end

A score here is not a quality rating. It is a normalized engineering signal representing how heavily a resource is being exercised.

Regression engine
julia/Regression.jl
module SurfaceRegression

using LinearAlgebra

export linear_fit, predict

struct LinearModel
    coefficients::Vector{Float64}
end

function linear_fit(
    x::Vector{Float64},
    y::Vector{Float64}
)
    X = hcat(
        ones(length(x)),
        x
    )

    coefficients =
        X \ y

    return LinearModel(coefficients)
end

function predict(
    model::LinearModel,
    x::Vector{Float64}
)
    X = hcat(
        ones(length(x)),
        x
    )

    return X * model.coefficients
end

end

This can be used to answer engineering questions such as:

How much does temperature rise per
additional 10% CPU utilisation?

How does battery power change with
CPU frequency?

How much does GPU load contribute
to total system power?

Does display refresh rate materially
change battery consumption?
Performance/thermal relationship
julia/Forecast.jl
module SurfaceForecast

using Statistics

export temperature_from_cpu_load

function temperature_from_cpu_load(
    cpu_load::Vector{Float64},
    temperature::Vector{Float64}
)
    X = hcat(
        ones(length(cpu_load)),
        cpu_load
    )

    coefficients =
        X \ temperature

    return coefficients
end

function predict_temperature(
    coefficients,
    cpu_load::Real
)
    return coefficients[1] +
           coefficients[2] *
           cpu_load
end

end

Example:

coefficients =
    temperature_from_cpu_load(
        Float64.(df.cpu_percent),
        Float64.(df.cpu_temperature_c)
    )

predicted =
    predict_temperature(
        coefficients,
        75.0
    )

That creates a simple empirical thermal model from actual engineering captures.

Optimisation model
julia/Optimizer.jl
module SurfaceOptimizer

export find_power_efficient_point

function find_power_efficient_point(
    frequencies::Vector{Float64},
    performance::Vector{Float64},
    power::Vector{Float64}
)
    best_index = 1
    best_efficiency = -Inf

    for i in eachindex(frequencies)
        if power[i] <= 0
            continue
        end

        efficiency =
            performance[i] /
            power[i]

        if efficiency > best_efficiency
            best_efficiency = efficiency
            best_index = i
        end
    end

    return (
        frequency_mhz =
            frequencies[best_index],

        performance =
            performance[best_index],

        power_watts =
            power[best_index],

        efficiency =
            best_efficiency
    )
end

end

This is particularly useful for the Surface performance team because it can search for an efficiency operating point, rather than simply maximising clock frequency.

Master analytics pipeline
julia/AnalysisPipeline.jl
module SurfaceAnalysisPipeline

using Statistics

include("Statistics.jl")
include("ThermalModel.jl")
include("PowerModel.jl")
include("PerformanceModel.jl")
include("BottleneckModel.jl")

using .SurfaceStatistics
using .ThermalModel
using .PowerModel
using .PerformanceModel
using .BottleneckModel

export analyze

function analyze(df)
    metrics = Dict{String,Float64}()

    for (key, value) in
        thermal_metrics(df)
        metrics[key] = value
    end

    for (key, value) in
        power_metrics(df)
        metrics[key] = value
    end

    for (key, value) in
        performance_metrics(df)
        metrics[key] = value
    end

    bottleneck =
        calculate_bottleneck(df)

    return (
        metrics=metrics,
        bottleneck=bottleneck
    )
end

end
Main Julia module
julia/SurfaceAnalytics.jl
module SurfaceAnalytics

include("Types.jl")
include("Loader.jl")
include("Statistics.jl")
include("ThermalModel.jl")
include("PowerModel.jl")
include("PerformanceModel.jl")
include("BottleneckModel.jl")
include("Regression.jl")
include("Forecast.jl")
include("Optimizer.jl")
include("AnalysisPipeline.jl")

using .SurfaceLoader
using .SurfaceAnalysisPipeline

export load_telemetry
export analyze

end
Python visualisation layer

Julia should calculate the engineering quantities; Python turns them into things engineers can actually inspect.

python/load_data.py
from __future__ import annotations

import pandas as pd


def load_telemetry(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)

    df["timestamp"] = pd.to_datetime(
        df["timestamp"]
    )

    df = df.sort_values("timestamp")

    return df


def validate_telemetry(
    df: pd.DataFrame,
) -> list[str]:
    required = [
        "timestamp",
        "battery_percent",
        "battery_power_watts",
        "cpu_percent",
        "gpu_percent",
        "cpu_temperature_c",
        "gpu_temperature_c",
        "memory_used_percent",
    ]

    return [
        column
        for column in required
        if column not in df.columns
    ]
Visualisation
python/visualise.py
from __future__ import annotations

import matplotlib.pyplot as plt
import pandas as pd


def plot_thermal(
    df: pd.DataFrame,
    output: str,
) -> None:
    fig, ax = plt.subplots(
        figsize=(12, 6)
    )

    ax.plot(
        df["timestamp"],
        df["cpu_temperature_c"],
        label="CPU",
    )

    ax.plot(
        df["timestamp"],
        df["gpu_temperature_c"],
        label="GPU",
    )

    ax.set_title(
        "Surface Thermal Behaviour"
    )

    ax.set_ylabel(
        "Temperature (°C)"
    )

    ax.legend()

    fig.autofmt_xdate()
    fig.tight_layout()

    fig.savefig(
        output,
        dpi=160,
    )

    plt.close(fig)


def plot_utilisation(
    df: pd.DataFrame,
    output: str,
) -> None:
    fig, ax = plt.subplots(
        figsize=(12, 6)
    )

    ax.plot(
        df["timestamp"],
        df["cpu_percent"],
        label="CPU",
    )

    ax.plot(
        df["timestamp"],
        df["gpu_percent"],
        label="GPU",
    )

    ax.plot(
        df["timestamp"],
        df["npu_percent"],
        label="NPU",
    )

    ax.set_title(
        "Surface Compute Utilisation"
    )

    ax.set_ylabel(
        "Utilisation (%)"
    )

    ax.set_ylim(0, 100)

    ax.legend()

    fig.autofmt_xdate()
    fig.tight_layout()

    fig.savefig(
        output,
        dpi=160,
    )

    plt.close(fig)


def plot_power(
    df: pd.DataFrame,
    output: str,
) -> None:
    fig, ax = plt.subplots(
        figsize=(12, 6)
    )

    ax.plot(
        df["timestamp"],
        df["battery_power_watts"],
    )

    ax.set_title(
        "Surface Battery Power"
    )

    ax.set_ylabel(
        "Power (W)"
    )

    fig.autofmt_xdate()
    fig.tight_layout()

    fig.savefig(
        output,
        dpi=160,
    )

    plt.close(fig)
Correlation analysis
python/dashboard.py
from __future__ import annotations

import pandas as pd


def correlation_matrix(
    df: pd.DataFrame,
) -> pd.DataFrame:
    columns = [
        "battery_power_watts",
        "cpu_percent",
        "gpu_percent",
        "npu_percent",
        "cpu_temperature_c",
        "gpu_temperature_c",
        "memory_used_percent",
        "display_refresh_hz",
        "display_brightness_percent",
    ]

    available = [
        c for c in columns
        if c in df.columns
    ]

    return df[available].corr()


def print_summary(
    df: pd.DataFrame,
) -> None:
    print("=" * 60)
    print("SURFACE PERFORMANCE ANALYTICS")
    print("=" * 60)

    print(f"Samples: {len(df)}")

    print(
        f"CPU mean: "
        f"{df.cpu_percent.mean():.2f}%"
    )

    print(
        f"CPU peak: "
        f"{df.cpu_percent.max():.2f}%"
    )

    print(
        f"CPU temperature peak: "
        f"{df.cpu_temperature_c.max():.2f} °C"
    )

    print(
        f"GPU mean: "
        f"{df.gpu_percent.mean():.2f}%"
    )

    print(
        f"Power mean: "
        f"{df.battery_power_watts.mean():.2f} W"
    )

    print()
    print("Correlation matrix")
    print(correlation_matrix(df))
Automated engineering report
python/report.py
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd


def build_report(
    df: pd.DataFrame,
) -> dict:
    report = {
        "sample_count": len(df),

        "cpu": {
            "mean_percent":
                float(df.cpu_percent.mean()),
            "peak_percent":
                float(df.cpu_percent.max()),
        },

        "gpu": {
            "mean_percent":
                float(df.gpu_percent.mean()),
            "peak_percent":
                float(df.gpu_percent.max()),
        },

        "thermal": {
            "cpu_peak_c":
                float(
                    df.cpu_temperature_c.max()
                ),
            "gpu_peak_c":
                float(
                    df.gpu_temperature_c.max()
                ),
        },

        "power": {
            "mean_watts":
                float(
                    df.battery_power_watts.mean()
                ),
            "peak_watts":
                float(
                    df.battery_power_watts.max()
                ),
        },
    }

    return report


def save_report(
    report: dict,
    path: str,
) -> None:
    Path(path).write_text(
        json.dumps(
            report,
            indent=2,
        ),
        encoding="utf-8",
    )
Python orchestration
python/run_analysis.py
from __future__ import annotations

import argparse
from pathlib import Path

from load_data import load_telemetry
from report import build_report, save_report
from visualise import (
    plot_power,
    plot_thermal,
    plot_utilisation,
)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Surface Performance Analytics"
        )
    )

    parser.add_argument(
        "input",
        help="Telemetry CSV file",
    )

    parser.add_argument(
        "--output",
        default="analysis-output",
    )

    args = parser.parse_args()

    output = Path(args.output)
    output.mkdir(
        parents=True,
        exist_ok=True,
    )

    df = load_telemetry(args.input)

    plot_thermal(
        df,
        str(output / "thermal.png"),
    )

    plot_utilisation(
        df,
        str(output / "utilisation.png"),
    )

    plot_power(
        df,
        str(output / "power.png"),
    )

    report = build_report(df)

    save_report(
        report,
        str(output / "report.json"),
    )

    print(
        f"Analysed {len(df)} telemetry samples."
    )

    print(
        f"Output: {output.resolve()}"
    )


if __name__ == "__main__":
    main()
Julia tests
tests/julia_tests.jl
using Test

include("../julia/Statistics.jl")
using .SurfaceStatistics

@testset "Surface Statistics" begin

    values = [
        10.0,
        20.0,
        30.0,
        40.0,
        50.0
    ]

    result =
        summarize(values)

    @test result["mean"] == 30.0
    @test result["minimum"] == 10.0
    @test result["maximum"] == 50.0
    @test result["median"] == 30.0
end
Python tests
tests/test_python.py
import pandas as pd

from python.report import build_report


def test_report_generation():
    df = pd.DataFrame({
        "cpu_percent": [20, 40, 60],
        "gpu_percent": [10, 30, 50],
        "cpu_temperature_c": [40, 60, 80],
        "gpu_temperature_c": [35, 50, 65],
        "battery_power_watts": [8, 12, 20],
    })

    report = build_report(df)

    assert report["sample_count"] == 3

    assert (
        report["cpu"]["peak_percent"] == 60
    )

    assert (
        report["thermal"]["cpu_peak_c"] == 80
    )
    
    
    
    
    
    
    
    Project
SurfaceGamingGraphics/
├── CMakeLists.txt
├── README.md
│
├── cpp/
│   ├── include/
│   │   ├── GraphicsTypes.hpp
│   │   ├── GPUDevice.hpp
│   │   ├── GPUAdapter.hpp
│   │   ├── CommandContext.hpp
│   │   ├── FramePacer.hpp
│   │   ├── RenderScaler.hpp
│   │   ├── ShaderManager.hpp
│   │   ├── GPUProfiler.hpp
│   │   ├── GamingPolicy.hpp
│   │   └── GamingEngine.hpp
│   │
│   └── src/
│       ├── GPUDevice.cpp
│       ├── GPUAdapter.cpp
│       ├── CommandContext.cpp
│       ├── FramePacer.cpp
│       ├── RenderScaler.cpp
│       ├── ShaderManager.cpp
│       ├── GPUProfiler.cpp
│       ├── GamingPolicy.cpp
│       ├── GamingEngine.cpp
│       └── main.cpp
│
├── shaders/
│   ├── FullscreenVS.hlsl
│   ├── TonemapCS.hlsl
│   ├── BrightnessCS.hlsl
│   ├── UpscaleCS.hlsl
│   └── ClearCS.hlsl
│
└── tests/
    └── GamingGraphicsTests.cpp
1. Core graphics types
cpp/include/GraphicsTypes.hpp
#pragma once

#include <cstdint>
#include <algorithm>

namespace surface::graphics {

enum class GPUVendor {
    Unknown,
    Microsoft,
    NVIDIA,
    AMD,
    Intel,
    Qualcomm
};

enum class QueueType {
    Graphics,
    Compute,
    Copy
};

enum class PresentMode {
    Immediate,
    VSync,
    Adaptive,
    VRR
};

enum class QualityLevel {
    Low,
    Medium,
    High,
    Ultra
};

struct GPUInfo {
    GPUVendor vendor{GPUVendor::Unknown};

    std::uint32_t vendorId{0};
    std::uint32_t deviceId{0};

    std::uint64_t dedicatedVideoMemory{0};
    std::uint64_t sharedSystemMemory{0};

    bool supportsRayTracing{false};
    bool supportsMeshShaders{false};
    bool supportsVariableRateShading{false};
};

struct GPUTelemetry {
    double utilizationPercent{0.0};

    double graphicsUtilizationPercent{0.0};
    double computeUtilizationPercent{0.0};

    double temperatureC{0.0};

    double powerWatts{0.0};

    std::uint64_t dedicatedMemoryUsed{0};
    std::uint64_t sharedMemoryUsed{0};

    double coreFrequencyMHz{0.0};
    double memoryFrequencyMHz{0.0};

    double frameTimeMs{0.0};
    double gpuFrameTimeMs{0.0};
    double cpuFrameTimeMs{0.0};

    std::uint64_t renderedFrames{0};
    std::uint64_t droppedFrames{0};
};

struct RenderConfiguration {
    std::uint32_t displayWidth{1920};
    std::uint32_t displayHeight{1080};

    std::uint32_t renderWidth{1920};
    std::uint32_t renderHeight{1080};

    double renderScale{1.0};

    QualityLevel quality{QualityLevel::High};

    bool hdr{false};
    bool rayTracing{false};
    bool variableRateShading{false};

    PresentMode presentMode{PresentMode::VRR};

    double targetFPS{120.0};
};

struct FrameTiming {
    double cpuFrameMs{0.0};
    double gpuFrameMs{0.0};
    double presentFrameMs{0.0};

    double frameIntervalMs{0.0};

    std::uint64_t frameIndex{0};
};

struct GamingPolicy {
    double targetFPS{120.0};

    double minimumRenderScale{0.50};
    double maximumRenderScale{1.00};

    double thermalLimitC{90.0};
    double criticalThermalLimitC{95.0};

    double maximumPowerWatts{35.0};

    bool allowDynamicResolution{true};
    bool allowVRS{true};
    bool allowRayTracing{true};
};

}
2. GPU adapter
cpp/include/GPUAdapter.hpp
#pragma once

#include "GraphicsTypes.hpp"

#include <string>

namespace surface::graphics {

class GPUAdapter {
public:
    GPUAdapter() = default;

    bool initialize();

    const GPUInfo& info() const noexcept;

    std::string description() const;

private:
    GPUInfo info_{};
    std::string description_;
};

}
cpp/src/GPUAdapter.cpp
#include "GPUAdapter.hpp"

#ifdef _WIN32
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace surface::graphics {

bool GPUAdapter::initialize() {

#ifdef _WIN32

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;

    HRESULT hr = CreateDXGIFactory2(
        0,
        IID_PPV_ARGS(&factory)
    );

    if (FAILED(hr)) {
        return false;
    }

    for (UINT index = 0;; ++index) {

        Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter;

        hr = factory->EnumAdapterByGpuPreference(
            index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)
        );

        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        if (FAILED(hr)) {
            continue;
        }

        DXGI_ADAPTER_DESC3 desc{};

        if (FAILED(adapter->GetDesc3(&desc))) {
            continue;
        }

        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) {
            continue;
        }

        info_.vendorId = desc.VendorId;
        info_.deviceId = desc.DeviceId;

        info_.dedicatedVideoMemory =
            desc.DedicatedVideoMemory;

        info_.sharedSystemMemory =
            desc.SharedSystemMemory;

        description_ = "Direct3D 12 GPU";

        switch (desc.VendorId) {

            case 0x10DE:
                info_.vendor = GPUVendor::NVIDIA;
                description_ = "NVIDIA GPU";
                break;

            case 0x1002:
                info_.vendor = GPUVendor::AMD;
                description_ = "AMD GPU";
                break;

            case 0x8086:
                info_.vendor = GPUVendor::Intel;
                description_ = "Intel GPU";
                break;

            case 0x1414:
                info_.vendor = GPUVendor::Microsoft;
                description_ = "Microsoft GPU";
                break;

            default:
                info_.vendor = GPUVendor::Unknown;
                break;
        }

        return true;
    }

#endif

    return false;
}

const GPUInfo& GPUAdapter::info() const noexcept {
    return info_;
}

std::string GPUAdapter::description() const {
    return description_;
}

}
3. Direct3D 12 device
cpp/include/GPUDevice.hpp
#pragma once

#include "GPUAdapter.hpp"

#ifdef _WIN32
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace surface::graphics {

class GPUDevice {
public:
    bool initialize();

    bool valid() const noexcept;

#ifdef _WIN32
    ID3D12Device* native() const noexcept;
#endif

    const GPUAdapter& adapter() const noexcept;

private:
    GPUAdapter adapter_;

#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
#endif
};

}
cpp/src/GPUDevice.cpp
#include "GPUDevice.hpp"

#ifdef _WIN32
#include <d3d12.h>
#endif

namespace surface::graphics {

bool GPUDevice::initialize() {

    if (!adapter_.initialize()) {
        return false;
    }

#ifdef _WIN32

    IDXGIAdapter* nativeAdapter = nullptr;

    // In production retain the selected DXGI adapter COM object.
    // This simplified implementation lets D3D12 choose the adapter.

    HRESULT hr = D3D12CreateDevice(
        nativeAdapter,
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&device_)
    );

    return SUCCEEDED(hr);

#else

    return false;

#endif
}

bool GPUDevice::valid() const noexcept {

#ifdef _WIN32
    return device_ != nullptr;
#else
    return false;
#endif
}

#ifdef _WIN32

ID3D12Device* GPUDevice::native() const noexcept {
    return device_.Get();
}

#endif

const GPUAdapter& GPUDevice::adapter() const noexcept {
    return adapter_;
}

}

For production, the selected IDXGIAdapter4 should be retained and passed directly to D3D12CreateDevice; the simplified code above intentionally avoids pretending that adapter lifetime management is complete.

4. Command context
cpp/include/CommandContext.hpp
#pragma once

#include "GraphicsTypes.hpp"

#ifdef _WIN32
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace surface::graphics {

class CommandContext {
public:
    explicit CommandContext(QueueType type);

    bool initialize(
#ifdef _WIN32
        ID3D12Device* device
#endif
    );

#ifdef _WIN32
    ID3D12GraphicsCommandList* commandList() const noexcept;
    ID3D12CommandQueue* queue() const noexcept;
#endif

    void reset();
    void close();

private:
    QueueType type_;

#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
#endif
};

}
cpp/src/CommandContext.cpp
#include "CommandContext.hpp"

namespace surface::graphics {

CommandContext::CommandContext(QueueType type)
    : type_(type) {
}

bool CommandContext::initialize(
#ifdef _WIN32
    ID3D12Device* device
#endif
) {

#ifdef _WIN32

    if (!device) {
        return false;
    }

    D3D12_COMMAND_LIST_TYPE d3dType =
        D3D12_COMMAND_LIST_TYPE_DIRECT;

    switch (type_) {

        case QueueType::Graphics:
            d3dType = D3D12_COMMAND_LIST_TYPE_DIRECT;
            break;

        case QueueType::Compute:
            d3dType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            break;

        case QueueType::Copy:
            d3dType = D3D12_COMMAND_LIST_TYPE_COPY;
            break;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = d3dType;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    if (FAILED(device->CreateCommandQueue(
        &queueDesc,
        IID_PPV_ARGS(&queue_)))) {
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(
        d3dType,
        IID_PPV_ARGS(&allocator_)))) {
        return false;
    }

    if (FAILED(device->CreateCommandList(
        0,
        d3dType,
        allocator_.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList_)))) {
        return false;
    }

    commandList_->Close();

    return true;

#else

    return false;

#endif
}

void CommandContext::reset() {

#ifdef _WIN32

    if (allocator_) {
        allocator_->Reset();
    }

    if (commandList_) {
        commandList_->Reset(
            allocator_.Get(),
            nullptr
        );
    }

#endif
}

void CommandContext::close() {

#ifdef _WIN32

    if (commandList_) {
        commandList_->Close();
    }

#endif
}

#ifdef _WIN32

ID3D12GraphicsCommandList*
CommandContext::commandList() const noexcept {
    return commandList_.Get();
}

ID3D12CommandQueue*
CommandContext::queue() const noexcept {
    return queue_.Get();
}

#endif

}
5. Dynamic resolution / render scaling

This is particularly useful on a Surface-class machine because the system can trade image resolution for GPU power, thermals and frame time.

cpp/include/RenderScaler.hpp
#pragma once

#include "GraphicsTypes.hpp"

namespace surface::graphics {

class RenderScaler {
public:
    explicit RenderScaler(
        const GamingPolicy& policy
    );

    void update(
        double gpuFrameTimeMs,
        double temperatureC,
        double powerWatts
    );

    double scale() const noexcept;

    RenderConfiguration apply(
        RenderConfiguration config
    ) const;

private:
    GamingPolicy policy_;

    double scale_{1.0};

    static constexpr double targetFrameMs(
        double fps
    ) noexcept {
        return 1000.0 / fps;
    }
};

}
cpp/src/RenderScaler.cpp
#include "RenderScaler.hpp"

#include <algorithm>

namespace surface::graphics {

RenderScaler::RenderScaler(
    const GamingPolicy& policy
)
    : policy_(policy) {
}

void RenderScaler::update(
    double gpuFrameTimeMs,
    double temperatureC,
    double powerWatts
) {

    if (!policy_.allowDynamicResolution) {
        return;
    }

    const double target =
        targetFrameMs(policy_.targetFPS);

    if (temperatureC >= policy_.criticalThermalLimitC ||
        powerWatts > policy_.maximumPowerWatts) {

        scale_ -= 0.05;

    } else if (gpuFrameTimeMs > target * 1.10) {

        scale_ -= 0.02;

    } else if (gpuFrameTimeMs < target * 0.85 &&
               temperatureC < policy_.thermalLimitC &&
               powerWatts < policy_.maximumPowerWatts * 0.90) {

        scale_ += 0.01;
    }

    scale_ = std::clamp(
        scale_,
        policy_.minimumRenderScale,
        policy_.maximumRenderScale
    );
}

double RenderScaler::scale() const noexcept {
    return scale_;
}

RenderConfiguration RenderScaler::apply(
    RenderConfiguration config
) const {

    config.renderScale = scale_;

    config.renderWidth =
        static_cast<std::uint32_t>(
            config.displayWidth * scale_);

    config.renderHeight =
        static_cast<std::uint32_t>(
            config.displayHeight * scale_);

    return config;
}

}
6. Frame pacing
cpp/include/FramePacer.hpp
#pragma once

#include "GraphicsTypes.hpp"

#include <chrono>
#include <deque>

namespace surface::graphics {

class FramePacer {
public:
    explicit FramePacer(double targetFPS);

    void beginFrame();

    FrameTiming endFrame(
        double gpuFrameMs
    );

    double averageFrameTimeMs() const noexcept;

    double estimatedFPS() const noexcept;

private:
    double targetFPS_;

    std::uint64_t frameIndex_{0};

    std::chrono::steady_clock::time_point
        frameStart_;

    std::deque<double> history_;

    static constexpr std::size_t
        historySize_ = 120;
};

}
cpp/src/FramePacer.cpp
#include "FramePacer.hpp"

#include <numeric>

namespace surface::graphics {

FramePacer::FramePacer(double targetFPS)
    : targetFPS_(targetFPS) {
}

void FramePacer::beginFrame() {
    frameStart_ =
        std::chrono::steady_clock::now();
}

FrameTiming FramePacer::endFrame(
    double gpuFrameMs
) {

    const auto now =
        std::chrono::steady_clock::now();

    const double cpuFrameMs =
        std::chrono::duration<double, std::milli>(
            now - frameStart_
        ).count();

    const double frameMs =
        std::max(cpuFrameMs, gpuFrameMs);

    history_.push_back(frameMs);

    if (history_.size() > historySize_) {
        history_.pop_front();
    }

    FrameTiming result;

    result.cpuFrameMs = cpuFrameMs;
    result.gpuFrameMs = gpuFrameMs;
    result.presentFrameMs = frameMs;
    result.frameIntervalMs = frameMs;
    result.frameIndex = frameIndex_++;

    return result;
}

double FramePacer::averageFrameTimeMs() const noexcept {

    if (history_.empty()) {
        return 0.0;
    }

    const double total =
        std::accumulate(
            history_.begin(),
            history_.end(),
            0.0
        );

    return total / history_.size();
}

double FramePacer::estimatedFPS() const noexcept {

    const double frame =
        averageFrameTimeMs();

    if (frame <= 0.0) {
        return 0.0;
    }

    return 1000.0 / frame;
}

}
7. GPU profiler
cpp/include/GPUProfiler.hpp
#pragma once

#include "GraphicsTypes.hpp"

#include <deque>

namespace surface::graphics {

class GPUProfiler {
public:
    void record(
        const GPUTelemetry& telemetry
    );

    GPUTelemetry latest() const;

    double averageGPUFrameTime() const;

    double averageUtilization() const;

private:
    std::deque<GPUTelemetry> samples_;

    static constexpr std::size_t
        maxSamples_ = 120;
};

}
cpp/src/GPUProfiler.cpp
#include "GPUProfiler.hpp"

namespace surface::graphics {

void GPUProfiler::record(
    const GPUTelemetry& telemetry
) {

    samples_.push_back(telemetry);

    if (samples_.size() > maxSamples_) {
        samples_.pop_front();
    }
}

GPUTelemetry GPUProfiler::latest() const {

    if (samples_.empty()) {
        return {};
    }

    return samples_.back();
}

double GPUProfiler::averageGPUFrameTime() const {

    if (samples_.empty()) {
        return 0.0;
    }

    double total = 0.0;

    for (const auto& sample : samples_) {
        total += sample.gpuFrameTimeMs;
    }

    return total / samples_.size();
}

double GPUProfiler::averageUtilization() const {

    if (samples_.empty()) {
        return 0.0;
    }

    double total = 0.0;

    for (const auto& sample : samples_) {
        total += sample.utilizationPercent;
    }

    return total / samples_.size();
}

}
8. Gaming policy engine
cpp/include/GamingPolicy.hpp
#pragma once

#include "GraphicsTypes.hpp"

namespace surface::graphics {

class GamingPolicyEngine {
public:
    explicit GamingPolicyEngine(
        GamingPolicy policy
    );

    RenderConfiguration evaluate(
        const GPUTelemetry& gpu,
        RenderConfiguration config
    ) const;

private:
    GamingPolicy policy_;
};

}
cpp/src/GamingPolicy.cpp
#include "GamingPolicy.hpp"

namespace surface::graphics {

GamingPolicyEngine::GamingPolicyEngine(
    GamingPolicy policy
)
    : policy_(policy) {
}

RenderConfiguration
GamingPolicyEngine::evaluate(
    const GPUTelemetry& gpu,
    RenderConfiguration config
) const {

    if (gpu.temperatureC >=
        policy_.criticalThermalLimitC) {

        config.rayTracing = false;
        config.variableRateShading = true;
        config.quality = QualityLevel::Medium;
    }

    else if (gpu.temperatureC >=
             policy_.thermalLimitC) {

        config.rayTracing = false;
        config.variableRateShading =
            policy_.allowVRS;

    }

    if (gpu.gpuFrameTimeMs >
        1000.0 / policy_.targetFPS) {

        config.variableRateShading =
            policy_.allowVRS;
    }

    return config;
}

}
9. Shader manager
cpp/include/ShaderManager.hpp
#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace surface::graphics {

class ShaderManager {
public:

    bool compile(
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const std::wstring& target
    );

#ifdef _WIN32

    ID3DBlob* bytecode() const noexcept;

#endif

private:

#ifdef _WIN32

    Microsoft::WRL::ComPtr<ID3DBlob>
        bytecode_;

    Microsoft::WRL::ComPtr<ID3DBlob>
        errors_;

#endif
};

}
cpp/src/ShaderManager.cpp
#include "ShaderManager.hpp"

#ifdef _WIN32
#include <d3dcompiler.h>
#endif

namespace surface::graphics {

bool ShaderManager::compile(
    const std::wstring& filename,
    const std::wstring& entryPoint,
    const std::wstring& target
) {

#ifdef _WIN32

    UINT flags =
        D3DCOMPILE_ENABLE_STRICTNESS;

#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
    flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = D3DCompileFromFile(
        filename.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        target.c_str(),
        flags,
        0,
        &bytecode_,
        &errors_
    );

    return SUCCEEDED(hr);

#else

    (void)filename;
    (void)entryPoint;
    (void)target;

    return false;

#endif
}

#ifdef _WIN32

ID3DBlob* ShaderManager::bytecode() const noexcept {
    return bytecode_.Get();
}

#endif

}
10. HLSL — fullscreen vertex shader
shaders/FullscreenVS.hlsl
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 uv[3] =
    {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    output.position =
        float4(positions[vertexID], 0.0, 1.0);

    output.uv = uv[vertexID];

    return output;
}

This uses the standard single fullscreen triangle technique, avoiding unnecessary vertex-buffer setup.

11. HLSL — compute shader
shaders/BrightnessCS.hlsl
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer Parameters : register(b0)
{
    float brightness;
    float contrast;
    float saturation;
    float padding;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;

    InputTexture.GetDimensions(width, height);

    if (id.x >= width || id.y >= height)
        return;

    float4 pixel =
        InputTexture[id.xy];

    float3 color =
        pixel.rgb * brightness;

    color =
        (color - 0.5) * contrast + 0.5;

    float luminance =
        dot(color, float3(
            0.2126,
            0.7152,
            0.0722
        ));

    color =
        lerp(
            luminance.xxx,
            color,
            saturation
        );

    OutputTexture[id.xy] =
        float4(
            saturate(color),
            pixel.a
        );
}
12. HLSL — tone mapping
shaders/TonemapCS.hlsl
Texture2D<float4> HDRInput : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer ToneMapParameters : register(b0)
{
    float exposure;
    float whitePoint;
    float gamma;
    float padding;
};

float3 ACESApprox(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;

    return saturate(
        (x * (a * x + b)) /
        (x * (c * x + d) + e)
    );
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;

    HDRInput.GetDimensions(width, height);

    if (id.x >= width || id.y >= height)
        return;

    float3 hdr =
        HDRInput[id.xy].rgb;

    hdr *= exposure;

    float3 mapped =
        ACESApprox(hdr);

    mapped =
        pow(
            max(mapped, 0.0),
            1.0 / gamma
        );

    Output[id.xy] =
        float4(mapped, 1.0);
}
13. HLSL — simple GPU upscale
shaders/UpscaleCS.hlsl
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer UpscaleParameters : register(b0)
{
    uint inputWidth;
    uint inputHeight;

    uint outputWidth;
    uint outputHeight;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outputWidth ||
        id.y >= outputHeight)
        return;

    float2 uv =
        (float2(id.xy) + 0.5) /
        float2(outputWidth, outputHeight);

    float2 source =
        uv *
        float2(inputWidth, inputHeight) -
        0.5;

    int2 p =
        int2(floor(source));

    float2 f =
        frac(source);

    p.x = clamp(p.x, 0, int(inputWidth) - 1);
    p.y = clamp(p.y, 0, int(inputHeight) - 1);

    int2 p10 =
        int2(
            min(p.x + 1, int(inputWidth) - 1),
            p.y
        );

    int2 p01 =
        int2(
            p.x,
            min(p.y + 1, int(inputHeight) - 1)
        );

    int2 p11 =
        int2(
            min(p.x + 1, int(inputWidth) - 1),
            min(p.y + 1, int(inputHeight) - 1)
        );

    float4 a = InputTexture[p];
    float4 b = InputTexture[p10];
    float4 c = InputTexture[p01];
    float4 d = InputTexture[p11];

    float4 result =
        lerp(
            lerp(a, b, f.x),
            lerp(c, d, f.x),
            f.y
        );

    OutputTexture[id.xy] = result;
}

This is deliberately a baseline. A production Surface gaming stack could replace this with a much more sophisticated temporal/upscaling implementation.

14. Gaming engine
cpp/include/GamingEngine.hpp
#pragma once

#include "GPUDevice.hpp"
#include "FramePacer.hpp"
#include "GPUProfiler.hpp"
#include "RenderScaler.hpp"
#include "GamingPolicy.hpp"

namespace surface::graphics {

class GamingEngine {
public:
    explicit GamingEngine(
        GamingPolicy policy
    );

    bool initialize();

    void beginFrame();

    void updateGPU(
        const GPUTelemetry& telemetry
    );

    FrameTiming endFrame();

    RenderConfiguration
    optimize(
        RenderConfiguration config
    ) const;

    const GPUDevice& gpu() const noexcept;

    const RenderScaler& scaler() const noexcept;

private:
    GamingPolicy policy_;

    GPUDevice gpu_;

    FramePacer framePacer_;

    GPUProfiler profiler_;

    RenderScaler scaler_;

    GamingPolicyEngine policyEngine_;

    GPUTelemetry latestGPU_{};
};

}
cpp/src/GamingEngine.cpp
#include "GamingEngine.hpp"

namespace surface::graphics {

GamingEngine::GamingEngine(
    GamingPolicy policy
)
    : policy_(policy),
      framePacer_(policy.targetFPS),
      scaler_(policy),
      policyEngine_(policy) {
}

bool GamingEngine::initialize() {
    return gpu_.initialize();
}

void GamingEngine::beginFrame() {
    framePacer_.beginFrame();
}

void GamingEngine::updateGPU(
    const GPUTelemetry& telemetry
) {

    latestGPU_ = telemetry;

    profiler_.record(telemetry);

    scaler_.update(
        telemetry.gpuFrameTimeMs,
        telemetry.temperatureC,
        telemetry.powerWatts
    );
}

FrameTiming GamingEngine::endFrame() {

    return framePacer_.endFrame(
        latestGPU_.gpuFrameTimeMs
    );
}

RenderConfiguration GamingEngine::optimize(
    RenderConfiguration config
) const {

    config =
        policyEngine_.evaluate(
            latestGPU_,
            config
        );

    return scaler_.apply(config);
}

const GPUDevice&
GamingEngine::gpu() const noexcept {
    return gpu_;
}

const RenderScaler&
GamingEngine::scaler() const noexcept {
    return scaler_;
}

}
15. Example application
cpp/src/main.cpp
#include "GamingEngine.hpp"

#include <iostream>

using namespace surface::graphics;

int main() {

    GamingPolicy policy;

    policy.targetFPS = 120.0;
    policy.minimumRenderScale = 0.50;
    policy.maximumRenderScale = 1.00;

    GamingEngine engine(policy);

    if (!engine.initialize()) {

        std::cerr
            << "Unable to initialise Direct3D 12 GPU\n";

        return 1;
    }

    RenderConfiguration config;

    config.displayWidth = 2880;
    config.displayHeight = 1920;

    config.targetFPS = 120.0;

    GPUTelemetry telemetry;

    telemetry.utilizationPercent = 94.0;
    telemetry.gpuFrameTimeMs = 10.5;
    telemetry.temperatureC = 83.0;
    telemetry.powerWatts = 31.0;

    engine.beginFrame();

    engine.updateGPU(telemetry);

    RenderConfiguration optimized =
        engine.optimize(config);

    FrameTiming frame =
        engine.endFrame();

    std::cout
        << "GPU: "
        << engine.gpu().adapter().description()
        << '\n';

    std::cout
        << "Render scale: "
        << optimized.renderScale
        << '\n';

    std::cout
        << "Render resolution: "
        << optimized.renderWidth
        << "x"
        << optimized.renderHeight
        << '\n';

    std::cout
        << "Frame time: "
        << frame.presentFrameMs
        << " ms\n";

    return 0;
}
16. Tests
tests/GamingGraphicsTests.cpp
#include "RenderScaler.hpp"
#include "FramePacer.hpp"
#include "GamingPolicy.hpp"

#include <cassert>
#include <iostream>

using namespace surface::graphics;

void testDynamicResolutionReducesScale() {

    GamingPolicy policy;

    policy.targetFPS = 120.0;
    policy.minimumRenderScale = 0.50;
    policy.maximumRenderScale = 1.00;

    RenderScaler scaler(policy);

    for (int i = 0; i < 20; ++i) {

        scaler.update(
            12.0,
            92.0,
            40.0
        );
    }

    assert(
        scaler.scale() < 1.0
    );

    assert(
        scaler.scale() >=
        policy.minimumRenderScale
    );
}

void testRenderDimensions() {

    GamingPolicy policy;

    RenderScaler scaler(policy);

    RenderConfiguration config;

    config.displayWidth = 1920;
    config.displayHeight = 1080;

    config.renderScale = scaler.scale();

    auto result =
        scaler.apply(config);

    assert(result.renderWidth == 1920);
    assert(result.renderHeight == 1080);
}

void testFramePacer() {

    FramePacer pacer(120.0);

    for (int i = 0; i < 10; ++i) {

        pacer.beginFrame();

        auto timing =
            pacer.endFrame(8.333);

        assert(
            timing.gpuFrameMs > 0.0
        );
    }

    assert(
        pacer.estimatedFPS() > 0.0
    );
}

int main() {

    testDynamicResolutionReducesScale();
    testRenderDimensions();
    testFramePacer();

    std::cout
        << "Gaming graphics tests passed.\n";

    return 0;
}
17. CMake
CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

project(
    SurfaceGamingGraphics
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
    SurfaceGamingGraphics
    STATIC

    cpp/src/GPUAdapter.cpp
    cpp/src/GPUDevice.cpp
    cpp/src/CommandContext.cpp
    cpp/src/FramePacer.cpp
    cpp/src/RenderScaler.cpp
    cpp/src/ShaderManager.cpp
    cpp/src/GPUProfiler.cpp
    cpp/src/GamingPolicy.cpp
    cpp/src/GamingEngine.cpp
)

target_include_directories(
    SurfaceGamingGraphics
    PUBLIC
    cpp/include
)

if(WIN32)

    target_link_libraries(
        SurfaceGamingGraphics
        PRIVATE
        d3d12
        dxgi
        dxguid
        d3dcompiler
    )

endif()

add_executable(
    SurfaceGamingDemo
    cpp/src/main.cpp
)

target_link_libraries(
    SurfaceGamingDemo
    PRIVATE
    SurfaceGamingGraphics
)

add_executable(
    SurfaceGamingTests
    tests/GamingGraphicsTests.cpp
)

target_link_libraries(
    SurfaceGamingTests
    PRIVATE
    SurfaceGamingGraphics
)

enable_testing()

add_test(
    NAME GamingGraphicsTests
    COMMAND SurfaceGamingTests
)






Project
SurfaceWebExperience/
├── package.json
├── tsconfig.json
├── vite.config.ts
├── index.html
├── README.md
│
├── src/
│   ├── main.tsx
│   ├── App.tsx
│   ├── styles.css
│   │
│   ├── api/
│   │   ├── client.ts
│   │   ├── devices.ts
│   │   └── telemetry.ts
│   │
│   ├── models/
│   │   ├── device.ts
│   │   ├── telemetry.ts
│   │   └── service.ts
│   │
│   ├── services/
│   │   ├── websocket.ts
│   │   └── deviceService.ts
│   │
│   ├── state/
│   │   └── surfaceStore.ts
│   │
│   ├── components/
│   │   ├── Layout.tsx
│   │   ├── Sidebar.tsx
│   │   ├── StatusCard.tsx
│   │   ├── MetricCard.tsx
│   │   ├── TemperatureCard.tsx
│   │   ├── BatteryCard.tsx
│   │   ├── PerformanceCard.tsx
│   │   ├── RadioCard.tsx
│   │   └── ServiceTable.tsx
│   │
│   └── pages/
│       ├── Dashboard.tsx
│       ├── Diagnostics.tsx
│       ├── Performance.tsx
│       ├── Gaming.tsx
│       └── Settings.tsx
│
└── tests/
    ├── telemetry.test.ts
    └── api.test.ts
1. Package configuration
package.json
{
  "name": "surface-web-experience",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc && vite build",
    "preview": "vite preview",
    "test": "vitest",
    "test:watch": "vitest --watch"
  },
  "dependencies": {
    "react": "^19.0.0",
    "react-dom": "^19.0.0",
    "react-router-dom": "^7.0.0"
  },
  "devDependencies": {
    "@types/react": "^19.0.0",
    "@types/react-dom": "^19.0.0",
    "@vitejs/plugin-react": "^4.0.0",
    "typescript": "^5.0.0",
    "vite": "^7.0.0",
    "vitest": "^3.0.0"
  }
}
2. TypeScript configuration
tsconfig.json
{
  "compilerOptions": {
    "target": "ES2022",
    "useDefineForClassFields": true,
    "lib": [
      "ES2022",
      "DOM",
      "DOM.Iterable"
    ],
    "allowJs": false,
    "skipLibCheck": true,
    "esModuleInterop": true,
    "allowSyntheticDefaultImports": true,
    "strict": true,
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "resolveJsonModule": true,
    "isolatedModules": true,
    "jsx": "react-jsx",
    "noEmit": true
  },
  "include": [
    "src",
    "tests"
  ]
}
3. Device model
src/models/device.ts
export type DeviceState =
  | "online"
  | "sleeping"
  | "offline"
  | "degraded";

export interface SurfaceDevice {
  id: string;
  name: string;
  model: string;
  serialMasked: string;

  state: DeviceState;

  batteryPercent: number;
  charging: boolean;

  cpuTemperatureC: number;
  gpuTemperatureC: number;

  cpuUtilization: number;
  gpuUtilization: number;
  npuUtilization: number;

  memoryUsedGB: number;
  memoryTotalGB: number;

  wifiRssi: number;

  displayRefreshRate: number;
}
4. Telemetry model
src/models/telemetry.ts
export interface TelemetrySample {
  timestamp: string;

  batteryPercent: number;
  powerWatts: number;

  cpuUtilization: number;
  gpuUtilization: number;
  npuUtilization: number;

  cpuTemperatureC: number;
  gpuTemperatureC: number;

  cpuFrequencyMHz: number;
  gpuFrequencyMHz: number;

  memoryUsedGB: number;
  memoryTotalGB: number;

  wifiRssi: number;
  wifiLatencyMs: number;
  wifiPacketLossPercent: number;

  frameTimeMs: number;
  gpuFrameTimeMs: number;
  fps: number;
}
5. Service model
src/models/service.ts
export type ServiceState =
  | "running"
  | "stopped"
  | "failed"
  | "starting";

export interface SurfaceService {
  id: string;
  name: string;
  state: ServiceState;

  uptimeSeconds: number;

  restartCount: number;

  lastHeartbeat: string;
}
6. Typed API client
src/api/client.ts
export class ApiError extends Error {
  constructor(
    message: string,
    public readonly status: number
  ) {
    super(message);
    this.name = "ApiError";
  }
}

export class SurfaceApiClient {

  constructor(
    private readonly baseUrl = "/api"
  ) {}

  async get<T>(path: string): Promise<T> {

    const response =
      await fetch(`${this.baseUrl}${path}`, {
        headers: {
          "Accept": "application/json"
        }
      });

    if (!response.ok) {
      throw new ApiError(
        `Request failed: ${response.status}`,
        response.status
      );
    }

    return response.json() as Promise<T>;
  }

  async post<T>(
    path: string,
    body: unknown
  ): Promise<T> {

    const response =
      await fetch(`${this.baseUrl}${path}`, {
        method: "POST",

        headers: {
          "Content-Type": "application/json",
          "Accept": "application/json"
        },

        body: JSON.stringify(body)
      });

    if (!response.ok) {
      throw new ApiError(
        `Request failed: ${response.status}`,
        response.status
      );
    }

    return response.json() as Promise<T>;
  }
}
7. Device API
src/api/devices.ts
import type { SurfaceDevice } from "../models/device";
import { SurfaceApiClient } from "./client";

export class DeviceApi {

  constructor(
    private readonly client: SurfaceApiClient
  ) {}

  getCurrentDevice(): Promise<SurfaceDevice> {
    return this.client.get<SurfaceDevice>(
      "/device"
    );
  }

  restartService(
    serviceId: string
  ): Promise<void> {

    return this.client.post<void>(
      `/services/${encodeURIComponent(serviceId)}/restart`,
      {}
    );
  }
}
8. WebSocket telemetry
src/services/websocket.ts
import type { TelemetrySample } from "../models/telemetry";

export type TelemetryHandler =
  (sample: TelemetrySample) => void;

export class TelemetrySocket {

  private socket?: WebSocket;

  private reconnectTimer?: number;

  constructor(
    private readonly url: string,
    private readonly onSample: TelemetryHandler
  ) {}

  connect(): void {

    this.socket =
      new WebSocket(this.url);

    this.socket.onmessage = event => {

      try {

        const sample =
          JSON.parse(
            event.data
          ) as TelemetrySample;

        this.onSample(sample);

      } catch (error) {

        console.error(
          "Invalid telemetry message",
          error
        );
      }
    };

    this.socket.onclose = () => {
      this.scheduleReconnect();
    };
  }

  disconnect(): void {

    if (this.reconnectTimer !== undefined) {
      window.clearTimeout(
        this.reconnectTimer
      );
    }

    this.socket?.close();
  }

  private scheduleReconnect(): void {

    this.reconnectTimer =
      window.setTimeout(
        () => this.connect(),
        2000
      );
  }
}
9. Surface state
src/state/surfaceStore.ts
import type { SurfaceDevice } from "../models/device";
import type { TelemetrySample } from "../models/telemetry";

export interface SurfaceState {
  device: SurfaceDevice | null;

  telemetry: TelemetrySample[];

  connected: boolean;
}

export const initialSurfaceState:
  SurfaceState = {

    device: null,

    telemetry: [],

    connected: false
};

export function addTelemetry(
  state: SurfaceState,
  sample: TelemetrySample
): SurfaceState {

  const telemetry = [
    ...state.telemetry,
    sample
  ].slice(-120);

  return {
    ...state,
    telemetry
  };
}
10. Main application
src/App.tsx
import {
  BrowserRouter,
  Routes,
  Route
} from "react-router-dom";

import { Layout } from "./components/Layout";

import { Dashboard }
  from "./pages/Dashboard";

import { Diagnostics }
  from "./pages/Diagnostics";

import { Performance }
  from "./pages/Performance";

import { Gaming }
  from "./pages/Gaming";

import { Settings }
  from "./pages/Settings";

export function App() {

  return (
    <BrowserRouter>

      <Layout>

        <Routes>

          <Route
            path="/"
            element={<Dashboard />}
          />

          <Route
            path="/diagnostics"
            element={<Diagnostics />}
          />

          <Route
            path="/performance"
            element={<Performance />}
          />

          <Route
            path="/gaming"
            element={<Gaming />}
          />

          <Route
            path="/settings"
            element={<Settings />}
          />

        </Routes>

      </Layout>

    </BrowserRouter>
  );
}
11. Application entry point
src/main.tsx
import React from "react";
import ReactDOM from "react-dom/client";

import { App } from "./App";

import "./styles.css";

ReactDOM.createRoot(
  document.getElementById("root")!
).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
12. Navigation
src/components/Sidebar.tsx
import { NavLink } from "react-router-dom";

const links = [
  {
    path: "/",
    label: "Overview"
  },
  {
    path: "/performance",
    label: "Performance"
  },
  {
    path: "/gaming",
    label: "Gaming"
  },
  {
    path: "/diagnostics",
    label: "Diagnostics"
  },
  {
    path: "/settings",
    label: "Settings"
  }
];

export function Sidebar() {

  return (
    <aside className="sidebar">

      <div className="brand">
        SURFACE
      </div>

      <nav>

        {links.map(link => (

          <NavLink
            key={link.path}
            to={link.path}
            className={({ isActive }) =>
              isActive
                ? "nav-link active"
                : "nav-link"
            }
          >
            {link.label}
          </NavLink>

        ))}

      </nav>

    </aside>
  );
}
13. Layout
src/components/Layout.tsx
import type { ReactNode } from "react";
import { Sidebar } from "./Sidebar";

interface Props {
  children: ReactNode;
}

export function Layout({
  children
}: Props) {

  return (
    <div className="app">

      <Sidebar />

      <main className="content">

        <header className="topbar">

          <div>
            Surface Control
          </div>

          <div className="connection">
            ● Device connected
          </div>

        </header>

        {children}

      </main>

    </div>
  );
}
14. Metric card
src/components/MetricCard.tsx
interface Props {
  label: string;
  value: string;
  detail?: string;
}

export function MetricCard({
  label,
  value,
  detail
}: Props) {

  return (
    <section className="metric-card">

      <div className="metric-label">
        {label}
      </div>

      <div className="metric-value">
        {value}
      </div>

      {detail && (
        <div className="metric-detail">
          {detail}
        </div>
      )}

    </section>
  );
}
15. Battery card
src/components/BatteryCard.tsx
interface Props {
  percentage: number;
  charging: boolean;
}

export function BatteryCard({
  percentage,
  charging
}: Props) {

  return (
    <section className="panel">

      <div className="panel-title">
        Battery
      </div>

      <div className="battery-value">
        {percentage}%
      </div>

      <div className="progress">

        <div
          className="progress-fill"
          style={{
            width: `${percentage}%`
          }}
        />

      </div>

      <div className="panel-detail">

        {charging
          ? "Charging"
          : "On battery"}

      </div>

    </section>
  );
}
16. Performance card
src/components/PerformanceCard.tsx
import type { TelemetrySample }
  from "../models/telemetry";

interface Props {
  telemetry: TelemetrySample;
}

export function PerformanceCard({
  telemetry
}: Props) {

  return (
    <section className="panel">

      <div className="panel-title">
        Performance
      </div>

      <div className="performance-grid">

        <div>
          <span>CPU</span>
          <strong>
            {telemetry.cpuUtilization.toFixed(0)}%
          </strong>
        </div>

        <div>
          <span>GPU</span>
          <strong>
            {telemetry.gpuUtilization.toFixed(0)}%
          </strong>
        </div>

        <div>
          <span>NPU</span>
          <strong>
            {telemetry.npuUtilization.toFixed(0)}%
          </strong>
        </div>

        <div>
          <span>FPS</span>
          <strong>
            {telemetry.fps.toFixed(0)}
          </strong>
        </div>

      </div>

    </section>
  );
}
17. Dashboard
src/pages/Dashboard.tsx
import { MetricCard }
  from "../components/MetricCard";

import { BatteryCard }
  from "../components/BatteryCard";

import { PerformanceCard }
  from "../components/PerformanceCard";

import type { TelemetrySample }
  from "../models/telemetry";

const demoTelemetry: TelemetrySample = {

  timestamp:
    new Date().toISOString(),

  batteryPercent: 82,
  powerWatts: 22.4,

  cpuUtilization: 31,
  gpuUtilization: 44,
  npuUtilization: 7,

  cpuTemperatureC: 61,
  gpuTemperatureC: 58,

  cpuFrequencyMHz: 3200,
  gpuFrequencyMHz: 1450,

  memoryUsedGB: 9.2,
  memoryTotalGB: 32,

  wifiRssi: -51,
  wifiLatencyMs: 11,
  wifiPacketLossPercent: 0.1,

  frameTimeMs: 8.4,
  gpuFrameTimeMs: 7.8,
  fps: 119
};

export function Dashboard() {

  return (
    <div className="page">

      <div className="page-heading">

        <div>
          <h1>Surface</h1>

          <p>
            Device overview
          </p>
        </div>

        <div className="device-status">
          ONLINE
        </div>

      </div>

      <div className="metrics">

        <MetricCard
          label="CPU temperature"
          value={`${demoTelemetry.cpuTemperatureC}°C`}
          detail="Normal operating range"
        />

        <MetricCard
          label="GPU temperature"
          value={`${demoTelemetry.gpuTemperatureC}°C`}
          detail="Graphics workload"
        />

        <MetricCard
          label="Power"
          value={`${demoTelemetry.powerWatts} W`}
          detail="Current system draw"
        />

        <MetricCard
          label="Wi-Fi"
          value={`${demoTelemetry.wifiLatencyMs} ms`}
          detail={`${demoTelemetry.wifiRssi} dBm`}
        />

      </div>

      <div className="dashboard-grid">

        <BatteryCard
          percentage={
            demoTelemetry.batteryPercent
          }
          charging={false}
        />

        <PerformanceCard
          telemetry={demoTelemetry}
        />

      </div>

    </div>
  );
}
18. Diagnostics page
src/pages/Diagnostics.tsx
import { useState } from "react";

export function Diagnostics() {

  const [running, setRunning] =
    useState(false);

  const runDiagnostics = async () => {

    setRunning(true);

    try {

      await fetch(
        "/api/diagnostics/run",
        {
          method: "POST"
        }
      );

    } finally {

      setRunning(false);
    }
  };

  return (
    <div className="page">

      <h1>Diagnostics</h1>

      <p className="subtitle">
        Surface engineering diagnostics
      </p>

      <section className="panel">

        <h2>
          System diagnostic
        </h2>

        <p>
          Check power, thermal, radio,
          graphics and device services.
        </p>

        <button
          onClick={runDiagnostics}
          disabled={running}
        >
          {running
            ? "Running..."
            : "Run diagnostics"}
        </button>

      </section>

    </div>
  );
}
19. Gaming page
src/pages/Gaming.tsx
import { MetricCard }
  from "../components/MetricCard";

export function Gaming() {

  return (
    <div className="page">

      <div className="page-heading">

        <div>
          <h1>Gaming</h1>

          <p>
            Real-time graphics optimisation
          </p>
        </div>

        <div className="device-status">
          GAME MODE
        </div>

      </div>

      <div className="metrics">

        <MetricCard
          label="Frame rate"
          value="119 FPS"
          detail="Target 120 FPS"
        />

        <MetricCard
          label="GPU frame time"
          value="7.8 ms"
          detail="Graphics workload"
        />

        <MetricCard
          label="Render scale"
          value="100%"
          detail="Dynamic resolution"
        />

        <MetricCard
          label="GPU power"
          value="31 W"
          detail="Current allocation"
        />

      </div>

      <section className="panel">

        <h2>
          Graphics configuration
        </h2>

        <div className="settings-row">
          <span>Dynamic resolution</span>
          <strong>Enabled</strong>
        </div>

        <div className="settings-row">
          <span>Variable rate shading</span>
          <strong>Enabled</strong>
        </div>

        <div className="settings-row">
          <span>Ray tracing</span>
          <strong>Adaptive</strong>
        </div>

        <div className="settings-row">
          <span>Presentation</span>
          <strong>VRR</strong>
        </div>

      </section>

    </div>
  );
}
20. Performance page
src/pages/Performance.tsx
import { MetricCard }
  from "../components/MetricCard";

export function Performance() {

  return (
    <div className="page">

      <h1>Performance</h1>

      <p className="subtitle">
        CPU, GPU, NPU and memory activity
      </p>

      <div className="metrics">

        <MetricCard
          label="CPU utilisation"
          value="31%"
        />

        <MetricCard
          label="GPU utilisation"
          value="44%"
        />

        <MetricCard
          label="NPU utilisation"
          value="7%"
        />

        <MetricCard
          label="Memory"
          value="9.2 / 32 GB"
        />

      </div>

      <section className="panel">

        <h2>Current frequencies</h2>

        <div className="settings-row">
          <span>CPU</span>
          <strong>3.20 GHz</strong>
        </div>

        <div className="settings-row">
          <span>GPU</span>
          <strong>1.45 GHz</strong>
        </div>

      </section>

    </div>
  );
}
21. Settings
src/pages/Settings.tsx
export function Settings() {

  return (
    <div className="page">

      <h1>Settings</h1>

      <p className="subtitle">
        Surface system configuration
      </p>

      <section className="panel">

        <div className="settings-row">
          <span>Performance mode</span>
          <strong>Balanced</strong>
        </div>

        <div className="settings-row">
          <span>Battery optimisation</span>
          <strong>Adaptive</strong>
        </div>

        <div className="settings-row">
          <span>Thermal policy</span>
          <strong>Automatic</strong>
        </div>

        <div className="settings-row">
          <span>Cloud synchronisation</span>
          <strong>Enabled</strong>
        </div>

      </section>

    </div>
  );
}
22. Surface visual system
src/styles.css
:root {
  font-family:
    Inter,
    "Segoe UI",
    system-ui,
    sans-serif;

  color: #181818;
  background: #f5f5f5;

  font-synthesis: none;
  text-rendering: optimizeLegibility;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  min-width: 320px;
}

.app {
  min-height: 100vh;
  display: flex;
}

.sidebar {
  width: 240px;
  min-height: 100vh;

  background: #111;

  color: white;

  padding: 28px 18px;
}

.brand {
  font-size: 21px;
  font-weight: 700;
  letter-spacing: 0.18em;

  margin-bottom: 40px;
}

.nav-link {
  display: block;

  padding: 12px 14px;

  margin-bottom: 4px;

  border-radius: 8px;

  color: #aaa;

  text-decoration: none;

  transition:
    background 120ms,
    color 120ms;
}

.nav-link:hover,
.nav-link.active {
  background: #272727;
  color: white;
}

.content {
  flex: 1;

  min-width: 0;

  background: #f5f5f5;
}

.topbar {
  height: 64px;

  padding:
    0 32px;

  display: flex;

  align-items: center;

  justify-content: space-between;

  background: white;

  border-bottom:
    1px solid #ddd;
}

.connection {
  font-size: 13px;
  color: #555;
}

.page {
  padding: 36px;

  max-width: 1400px;

  margin: auto;
}

.page-heading {
  display: flex;

  align-items: center;

  justify-content: space-between;

  margin-bottom: 30px;
}

h1 {
  font-size: 34px;

  margin:
    0 0 5px;

  letter-spacing:
    -0.035em;
}

h2 {
  font-size: 19px;

  margin-top: 0;
}

.subtitle,
.page-heading p {
  margin: 0;

  color: #777;
}

.device-status {
  padding:
    8px 12px;

  border:
    1px solid #ccc;

  border-radius: 999px;

  font-size: 12px;

  font-weight: 700;

  letter-spacing:
    0.08em;
}

.metrics {
  display: grid;

  grid-template-columns:
    repeat(4, 1fr);

  gap: 16px;

  margin-bottom: 18px;
}

.metric-card,
.panel {
  background: white;

  border:
    1px solid #dedede;

  border-radius: 14px;

  padding: 22px;
}

.metric-label {
  color: #777;

  font-size: 13px;

  margin-bottom: 12px;
}

.metric-value {
  font-size: 29px;

  font-weight: 650;

  letter-spacing:
    -0.035em;
}

.metric-detail,
.panel-detail {
  margin-top: 8px;

  color: #888;

  font-size: 12px;
}

.dashboard-grid {
  display: grid;

  grid-template-columns:
    1fr 1fr;

  gap: 18px;
}

.panel-title {
  font-size: 14px;

  font-weight: 650;

  margin-bottom: 14px;
}

.battery-value {
  font-size: 42px;

  font-weight: 650;

  letter-spacing:
    -0.04em;
}

.progress {
  height: 8px;

  background: #ddd;

  border-radius: 999px;

  overflow: hidden;

  margin-top: 20px;
}

.progress-fill {
  height: 100%;

  background: #111;

  border-radius: inherit;
}

.performance-grid {
  display: grid;

  grid-template-columns:
    repeat(4, 1fr);

  gap: 20px;
}

.performance-grid span {
  display: block;

  color: #888;

  font-size: 12px;

  margin-bottom: 6px;
}

.performance-grid strong {
  font-size: 22px;
}

.settings-row {
  display: flex;

  justify-content: space-between;

  align-items: center;

  padding: 17px 0;

  border-bottom:
    1px solid #eee;
}

.settings-row:last-child {
  border-bottom: none;
}

button {
  border: 0;

  border-radius: 8px;

  padding:
    11px 18px;

  background: #111;

  color: white;

  cursor: pointer;

  font-weight: 600;
}

button:disabled {
  opacity: 0.5;

  cursor: default;
}

@media (max-width: 900px) {

  .sidebar {
    width: 190px;
  }

  .metrics {
    grid-template-columns:
      repeat(2, 1fr);
  }

  .dashboard-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 600px) {

  .app {
    display: block;
  }

  .sidebar {
    width: 100%;

    min-height: auto;
  }

  .sidebar nav {
    display: flex;

    overflow-x: auto;
  }

  .nav-link {
    white-space: nowrap;
  }

  .metrics {
    grid-template-columns: 1fr;
  }

  .performance-grid {
    grid-template-columns:
      repeat(2, 1fr);
  }

  .page {
    padding: 20px;
  }
}
23. Web application tests
tests/telemetry.test.ts
import { describe, expect, it } from "vitest";

import {
  addTelemetry,
  initialSurfaceState
} from "../src/state/surfaceStore";

const sample = {
  timestamp: new Date().toISOString(),

  batteryPercent: 80,
  powerWatts: 20,

  cpuUtilization: 40,
  gpuUtilization: 60,
  npuUtilization: 10,

  cpuTemperatureC: 60,
  gpuTemperatureC: 58,

  cpuFrequencyMHz: 3000,
  gpuFrequencyMHz: 1400,

  memoryUsedGB: 8,
  memoryTotalGB: 32,

  wifiRssi: -50,
  wifiLatencyMs: 10,
  wifiPacketLossPercent: 0,

  frameTimeMs: 8.3,
  gpuFrameTimeMs: 7.9,
  fps: 120
};

describe(
  "Surface telemetry",
  () => {

    it(
      "stores telemetry samples",
      () => {

        const state =
          addTelemetry(
            initialSurfaceState,
            sample
          );

        expect(
          state.telemetry
        ).toHaveLength(1);

        expect(
          state.telemetry[0].fps
        ).toBe(120);
      }
    );

    it(
      "keeps only recent telemetry",
      () => {

        let state =
          initialSurfaceState;

        for (
          let i = 0;
          i < 200;
          i++
        ) {

          state =
            addTelemetry(
              state,
              sample
            );
        }

        expect(
          state.telemetry.length
        ).toBe(120);
      }
    );
  }
);
24. The important part: connect it to the Surface stack

This TypeScript layer should consume the systems we've built in #1–#24, rather than duplicating them.

                         SURFACE
                            │
       ┌────────────────────┼─────────────────────┐
       │                    │                     │
       ▼                    ▼                     ▼
   C++ Runtime          Rust Services        Windows APIs
       │                    │                     │
       └────────────────────┼─────────────────────┘
                            │
                       Telemetry Bus
                            │
             ┌──────────────┼───────────────┐
             ▼              ▼               ▼
           JSON           WebSocket        IPC
             │              │               │
             └──────────────┼───────────────┘
                            ▼
                    TypeScript Runtime
                            │
             ┌──────────────┼──────────────┐
             ▼              ▼              ▼
          Surface       Diagnostics      Gaming
          Dashboard       Portal         Console
             │              │              │
             └──────────────┼──────────────┘
                            ▼
                         Browser
                         
                         
                         
                         
                         
                         
                         
                         
                         
                         
                         
                         
                         
                         
Project architecture
SurfaceEmbeddedFirmware/
├── CMakeLists.txt
├── README.md
│
├── common/
│   ├── include/
│   │   ├── firmware_types.h
│   │   ├── firmware_config.h
│   │   ├── protocol.h
│   │   └── crc16.h
│   └── src/
│       └── crc16.c
│
├── core/
│   ├── include/
│   │   ├── controller.h
│   │   ├── scheduler.h
│   │   ├── watchdog.h
│   │   ├── state_machine.h
│   │   └── ring_buffer.h
│   └── src/
│       ├── controller.c
│       ├── scheduler.c
│       ├── watchdog.c
│       ├── state_machine.c
│       └── ring_buffer.c
│
├── power/
│   ├── include/
│   │   ├── power_controller.h
│   │   └── battery_monitor.h
│   └── src/
│       ├── power_controller.c
│       └── battery_monitor.c
│
├── thermal/
│   ├── include/
│   │   └── thermal_controller.h
│   └── src/
│       └── thermal_controller.c
│
├── input/
│   ├── include/
│   │   └── button_controller.h
│   └── src/
│       └── button_controller.c
│
├── drivers/
│   ├── include/
│   │   ├── adc.h
│   │   ├── gpio.h
│   │   ├── pwm.h
│   │   ├── i2c.h
│   │   └── watchdog_hw.h
│   └── src/
│       └── simulated_hal.c
│
├── main/
│   └── main.c
│
└── tests/
    ├── test_crc.c
    ├── test_power.c
    ├── test_thermal.c
    └── test_scheduler.c

The important design rule is:

No dynamic allocation, no unbounded loops, no filesystem, no network stack and no operating-system dependency in the deterministic core.

1. Firmware types
common/include/firmware_types.h
#ifndef SURFACE_FIRMWARE_TYPES_H
#define SURFACE_FIRMWARE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    FW_OK = 0,
    FW_ERROR = 1,
    FW_INVALID_ARGUMENT = 2,
    FW_TIMEOUT = 3,
    FW_OVER_TEMPERATURE = 4,
    FW_LOW_BATTERY = 5
} FirmwareStatus;

typedef enum
{
    POWER_STATE_OFF = 0,
    POWER_STATE_IDLE,
    POWER_STATE_ACTIVE,
    POWER_STATE_CHARGING,
    POWER_STATE_FAULT
} PowerState;

typedef enum
{
    THERMAL_NORMAL = 0,
    THERMAL_WARM,
    THERMAL_HOT,
    THERMAL_CRITICAL,
    THERMAL_EMERGENCY
} ThermalState;

typedef struct
{
    uint16_t battery_mv;
    uint16_t system_mv;

    int16_t battery_current_ma;

    uint8_t battery_percent;

    bool external_power;

} BatteryTelemetry;

typedef struct
{
    int16_t cpu_temperature_c;
    int16_t gpu_temperature_c;

    int16_t board_temperature_c;

    uint16_t fan_rpm;

    ThermalState state;

} ThermalTelemetry;

typedef struct
{
    PowerState power_state;

    BatteryTelemetry battery;

    ThermalTelemetry thermal;

    uint32_t uptime_ms;

    uint32_t fault_flags;

} FirmwareTelemetry;

#endif
2. Firmware configuration
common/include/firmware_config.h
#ifndef SURFACE_FIRMWARE_CONFIG_H
#define SURFACE_FIRMWARE_CONFIG_H

#define FW_TICK_MS                 10U

#define FW_BATTERY_LOW_PERCENT     10U
#define FW_BATTERY_CRITICAL        5U

#define FW_THERMAL_WARM_C          70
#define FW_THERMAL_HOT_C           85
#define FW_THERMAL_CRITICAL_C      95
#define FW_THERMAL_EMERGENCY_C     100

#define FW_FAN_MIN_RPM             0U
#define FW_FAN_MAX_RPM             6000U

#define FW_WATCHDOG_TIMEOUT_MS     100U

#define FW_COMMAND_QUEUE_SIZE      32U

#endif
3. CRC-16

Firmware communicating with the host should have a simple integrity mechanism around its command packets.

common/include/crc16.h
#ifndef SURFACE_CRC16_H
#define SURFACE_CRC16_H

#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt(
    const uint8_t *data,
    size_t length
);

#endif
common/src/crc16.c
#include "crc16.h"

uint16_t crc16_ccitt(
    const uint8_t *data,
    size_t length
)
{
    uint16_t crc = 0xFFFFU;

    if (data == 0) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {

        crc ^= (uint16_t)data[i] << 8;

        for (uint8_t bit = 0; bit < 8U; ++bit) {

            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)(
                    (crc << 1U) ^ 0x1021U
                );
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}
4. Host ↔ embedded protocol
common/include/protocol.h
#ifndef SURFACE_FIRMWARE_PROTOCOL_H
#define SURFACE_FIRMWARE_PROTOCOL_H

#include <stdint.h>

#define FW_PROTOCOL_VERSION 1U

typedef enum
{
    CMD_GET_TELEMETRY = 1,
    CMD_SET_FAN = 2,
    CMD_SET_POWER_STATE = 3,
    CMD_ENTER_LOW_POWER = 4,
    CMD_RESET_FAULTS = 5,
    CMD_PING = 6
} FirmwareCommand;

typedef struct
{
    uint8_t version;
    uint8_t command;
    uint16_t sequence;

    uint32_t argument;

    uint16_t crc;

} FirmwarePacket;

typedef struct
{
    uint8_t version;
    uint8_t status;

    uint16_t sequence;

    uint32_t value;

    uint16_t crc;

} FirmwareResponse;

#endif

A production protocol should additionally include packet lengths, capability negotiation, authenticated host commands where appropriate, monotonically increasing counters and explicit framing.

5. Ring buffer
core/include/ring_buffer.h
#ifndef SURFACE_RING_BUFFER_H
#define SURFACE_RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#define RING_BUFFER_CAPACITY 32U

typedef struct
{
    uint8_t data[RING_BUFFER_CAPACITY];

    volatile uint8_t head;
    volatile uint8_t tail;

} RingBuffer;

void ring_init(
    RingBuffer *ring
);

bool ring_push(
    RingBuffer *ring,
    uint8_t value
);

bool ring_pop(
    RingBuffer *ring,
    uint8_t *value
);

bool ring_empty(
    const RingBuffer *ring
);

bool ring_full(
    const RingBuffer *ring
);

#endif
core/src/ring_buffer.c
#include "ring_buffer.h"

void ring_init(
    RingBuffer *ring
)
{
    if (ring == 0) {
        return;
    }

    ring->head = 0U;
    ring->tail = 0U;
}

bool ring_empty(
    const RingBuffer *ring
)
{
    return ring->head == ring->tail;
}

bool ring_full(
    const RingBuffer *ring
)
{
    uint8_t next =
        (uint8_t)(
            (ring->head + 1U)
            % RING_BUFFER_CAPACITY
        );

    return next == ring->tail;
}

bool ring_push(
    RingBuffer *ring,
    uint8_t value
)
{
    if (ring == 0 || ring_full(ring)) {
        return false;
    }

    ring->data[ring->head] = value;

    ring->head =
        (uint8_t)(
            (ring->head + 1U)
            % RING_BUFFER_CAPACITY
        );

    return true;
}

bool ring_pop(
    RingBuffer *ring,
    uint8_t *value
)
{
    if (
        ring == 0 ||
        value == 0 ||
        ring_empty(ring)
    ) {
        return false;
    }

    *value = ring->data[ring->tail];

    ring->tail =
        (uint8_t)(
            (ring->tail + 1U)
            % RING_BUFFER_CAPACITY
        );

    return true;
}

For a real MCU, the exact concurrency semantics should be matched to the interrupt architecture. volatile alone is not a universal thread-safety mechanism.

6. Deterministic scheduler
core/include/scheduler.h
#ifndef SURFACE_SCHEDULER_H
#define SURFACE_SCHEDULER_H

#include <stdint.h>

typedef void (*SchedulerTask)(void);

typedef struct
{
    SchedulerTask task;

    uint32_t period_ms;
    uint32_t elapsed_ms;

} SchedulerEntry;

#define SCHEDULER_MAX_TASKS 16U

typedef struct
{
    SchedulerEntry tasks[SCHEDULER_MAX_TASKS];

    uint8_t count;

} Scheduler;

void scheduler_init(
    Scheduler *scheduler
);

int scheduler_add(
    Scheduler *scheduler,
    SchedulerTask task,
    uint32_t period_ms
);

void scheduler_tick(
    Scheduler *scheduler,
    uint32_t elapsed_ms
);

#endif
core/src/scheduler.c
#include "scheduler.h"

void scheduler_init(
    Scheduler *scheduler
)
{
    if (scheduler == 0) {
        return;
    }

    scheduler->count = 0U;

    for (uint8_t i = 0;
         i < SCHEDULER_MAX_TASKS;
         ++i) {

        scheduler->tasks[i].task = 0;
        scheduler->tasks[i].period_ms = 0U;
        scheduler->tasks[i].elapsed_ms = 0U;
    }
}

int scheduler_add(
    Scheduler *scheduler,
    SchedulerTask task,
    uint32_t period_ms
)
{
    if (
        scheduler == 0 ||
        task == 0 ||
        period_ms == 0U ||
        scheduler->count >= SCHEDULER_MAX_TASKS
    ) {
        return -1;
    }

    SchedulerEntry *entry =
        &scheduler->tasks[scheduler->count];

    entry->task = task;
    entry->period_ms = period_ms;
    entry->elapsed_ms = 0U;

    ++scheduler->count;

    return 0;
}

void scheduler_tick(
    Scheduler *scheduler,
    uint32_t elapsed_ms
)
{
    if (scheduler == 0) {
        return;
    }

    for (uint8_t i = 0;
         i < scheduler->count;
         ++i) {

        SchedulerEntry *entry =
            &scheduler->tasks[i];

        entry->elapsed_ms += elapsed_ms;

        if (
            entry->elapsed_ms >=
            entry->period_ms
        ) {

            entry->elapsed_ms = 0U;

            entry->task();
        }
    }
}
7. Watchdog
core/include/watchdog.h
#ifndef SURFACE_WATCHDOG_H
#define SURFACE_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t timeout_ms;
    uint32_t elapsed_ms;

    bool expired;

} FirmwareWatchdog;

void watchdog_init(
    FirmwareWatchdog *watchdog,
    uint32_t timeout_ms
);

void watchdog_tick(
    FirmwareWatchdog *watchdog,
    uint32_t elapsed_ms
);

void watchdog_kick(
    FirmwareWatchdog *watchdog
);

bool watchdog_expired(
    const FirmwareWatchdog *watchdog
);

#endif
core/src/watchdog.c
#include "watchdog.h"

void watchdog_init(
    FirmwareWatchdog *watchdog,
    uint32_t timeout_ms
)
{
    if (watchdog == 0) {
        return;
    }

    watchdog->timeout_ms = timeout_ms;
    watchdog->elapsed_ms = 0U;
    watchdog->expired = false;
}

void watchdog_tick(
    FirmwareWatchdog *watchdog,
    uint32_t elapsed_ms
)
{
    if (watchdog == 0) {
        return;
    }

    watchdog->elapsed_ms += elapsed_ms;

    if (
        watchdog->elapsed_ms >=
        watchdog->timeout_ms
    ) {
        watchdog->expired = true;
    }
}

void watchdog_kick(
    FirmwareWatchdog *watchdog
)
{
    if (watchdog == 0) {
        return;
    }

    watchdog->elapsed_ms = 0U;
    watchdog->expired = false;
}

bool watchdog_expired(
    const FirmwareWatchdog *watchdog
)
{
    return watchdog != 0 &&
           watchdog->expired;
}
8. Battery monitor
power/include/battery_monitor.h
#ifndef SURFACE_BATTERY_MONITOR_H
#define SURFACE_BATTERY_MONITOR_H

#include "firmware_types.h"

void battery_monitor_init(void);

FirmwareStatus battery_monitor_read(
    BatteryTelemetry *telemetry
);

#endif
power/src/battery_monitor.c
#include "battery_monitor.h"

#include "adc.h"

void battery_monitor_init(void)
{
    adc_init();
}

FirmwareStatus battery_monitor_read(
    BatteryTelemetry *telemetry
)
{
    if (telemetry == 0) {
        return FW_INVALID_ARGUMENT;
    }

    uint16_t batteryRaw =
        adc_read(0U);

    uint16_t systemRaw =
        adc_read(1U);

    /*
     * These conversion constants are deliberately
     * placeholders. Real Surface hardware requires
     * the board-specific ADC scaling and battery
     * gauge specification.
     */

    telemetry->battery_mv =
        batteryRaw;

    telemetry->system_mv =
        systemRaw;

    telemetry->battery_current_ma = 0;

    telemetry->battery_percent =
        (uint8_t)(
            batteryRaw > 4200U
                ? 100U
                : (batteryRaw / 42U)
        );

    telemetry->external_power = false;

    return FW_OK;
}

The voltage conversion above is not suitable for actual production hardware; the point is to show the deterministic interface. Actual battery fuel-gauge data should come from the specified gauge/PMIC.

9. Power controller
power/include/power_controller.h
#ifndef SURFACE_POWER_CONTROLLER_H
#define SURFACE_POWER_CONTROLLER_H

#include "firmware_types.h"

void power_controller_init(void);

PowerState power_controller_update(
    const BatteryTelemetry *battery,
    ThermalState thermal
);

#endif
power/src/power_controller.c
#include "power_controller.h"

#include "firmware_config.h"

void power_controller_init(void)
{
}

PowerState power_controller_update(
    const BatteryTelemetry *battery,
    ThermalState thermal
)
{
    if (battery == 0) {
        return POWER_STATE_FAULT;
    }

    if (
        thermal == THERMAL_EMERGENCY ||
        thermal == THERMAL_CRITICAL
    ) {
        return POWER_STATE_FAULT;
    }

    if (
        battery->battery_percent <=
        FW_BATTERY_CRITICAL
    ) {
        return POWER_STATE_OFF;
    }

    if (
        battery->battery_percent <=
        FW_BATTERY_LOW_PERCENT
    ) {
        return POWER_STATE_IDLE;
    }

    if (battery->external_power) {
        return POWER_STATE_CHARGING;
    }

    return POWER_STATE_ACTIVE;
}
10. Thermal controller
thermal/include/thermal_controller.h
#ifndef SURFACE_THERMAL_CONTROLLER_H
#define SURFACE_THERMAL_CONTROLLER_H

#include "firmware_types.h"

void thermal_controller_init(void);

ThermalState thermal_classify(
    int16_t cpu_c,
    int16_t gpu_c,
    int16_t board_c
);

uint16_t thermal_fan_target(
    ThermalState state
);

#endif
thermal/src/thermal_controller.c
#include "thermal_controller.h"

#include "firmware_config.h"

void thermal_controller_init(void)
{
}

ThermalState thermal_classify(
    int16_t cpu_c,
    int16_t gpu_c,
    int16_t board_c
)
{
    int16_t hottest = cpu_c;

    if (gpu_c > hottest) {
        hottest = gpu_c;
    }

    if (board_c > hottest) {
        hottest = board_c;
    }

    if (
        hottest >=
        FW_THERMAL_EMERGENCY_C
    ) {
        return THERMAL_EMERGENCY;
    }

    if (
        hottest >=
        FW_THERMAL_CRITICAL_C
    ) {
        return THERMAL_CRITICAL;
    }

    if (
        hottest >=
        FW_THERMAL_HOT_C
    ) {
        return THERMAL_HOT;
    }

    if (
        hottest >=
        FW_THERMAL_WARM_C
    ) {
        return THERMAL_WARM;
    }

    return THERMAL_NORMAL;
}

uint16_t thermal_fan_target(
    ThermalState state
)
{
    switch (state) {

        case THERMAL_NORMAL:
            return 0U;

        case THERMAL_WARM:
            return 1800U;

        case THERMAL_HOT:
            return 3500U;

        case THERMAL_CRITICAL:
            return 5000U;

        case THERMAL_EMERGENCY:
            return 6000U;

        default:
            return 6000U;
    }
}

This is the sort of logic that belongs on an embedded controller because it can continue operating even if Windows is frozen, rebooting or unavailable.

11. PWM fan driver
drivers/include/pwm.h
#ifndef SURFACE_PWM_H
#define SURFACE_PWM_H

#include <stdint.h>

void pwm_init(void);

void pwm_set_fan_rpm(
    uint16_t rpm
);

#endif
drivers/include/adc.h
#ifndef SURFACE_ADC_H
#define SURFACE_ADC_H

#include <stdint.h>

void adc_init(void);

uint16_t adc_read(
    uint8_t channel
);

#endif
drivers/include/gpio.h
#ifndef SURFACE_GPIO_H
#define SURFACE_GPIO_H

#include <stdint.h>
#include <stdbool.h>

void gpio_init(void);

bool gpio_read(
    uint32_t pin
);

void gpio_write(
    uint32_t pin,
    bool value
);

#endif
12. Button controller
input/include/button_controller.h
#ifndef SURFACE_BUTTON_CONTROLLER_H
#define SURFACE_BUTTON_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    BUTTON_RELEASED = 0,
    BUTTON_PRESSED
} ButtonState;

typedef struct
{
    ButtonState stable_state;
    ButtonState sampled_state;

    uint16_t debounce_ms;

} ButtonController;

void button_init(
    ButtonController *button
);

bool button_update(
    ButtonController *button,
    bool raw_pressed,
    uint16_t elapsed_ms
);

#endif
input/src/button_controller.c
#include "button_controller.h"

#define BUTTON_DEBOUNCE_MS 30U

void button_init(
    ButtonController *button
)
{
    if (button == 0) {
        return;
    }

    button->stable_state =
        BUTTON_RELEASED;

    button->sampled_state =
        BUTTON_RELEASED;

    button->debounce_ms = 0U;
}

bool button_update(
    ButtonController *button,
    bool raw_pressed,
    uint16_t elapsed_ms
)
{
    if (button == 0) {
        return false;
    }

    ButtonState sample =
        raw_pressed
            ? BUTTON_PRESSED
            : BUTTON_RELEASED;

    if (
        sample !=
        button->sampled_state
    ) {

        button->sampled_state = sample;
        button->debounce_ms = 0U;

        return false;
    }

    if (
        button->stable_state !=
        button->sampled_state
    ) {

        button->debounce_ms += elapsed_ms;

        if (
            button->debounce_ms >=
            BUTTON_DEBOUNCE_MS
        ) {

            button->stable_state =
                button->sampled_state;

            return true;
        }
    }

    return false;
}
13. Embedded controller
core/include/controller.h
#ifndef SURFACE_CONTROLLER_H
#define SURFACE_CONTROLLER_H

#include "firmware_types.h"

typedef struct
{
    FirmwareTelemetry telemetry;

    uint32_t tick_count;

} SurfaceController;

void controller_init(
    SurfaceController *controller
);

void controller_tick(
    SurfaceController *controller,
    uint32_t elapsed_ms
);

const FirmwareTelemetry *
controller_telemetry(
    const SurfaceController *controller
);

#endif
core/src/controller.c
#include "controller.h"

#include "battery_monitor.h"
#include "power_controller.h"
#include "thermal_controller.h"
#include "firmware_config.h"

void controller_init(
    SurfaceController *controller
)
{
    if (controller == 0) {
        return;
    }

    controller->telemetry.power_state =
        POWER_STATE_OFF;

    controller->telemetry.uptime_ms = 0U;

    controller->telemetry.fault_flags = 0U;

    battery_monitor_init();
    power_controller_init();
    thermal_controller_init();

    controller->tick_count = 0U;
}

void controller_tick(
    SurfaceController *controller,
    uint32_t elapsed_ms
)
{
    if (controller == 0) {
        return;
    }

    controller->telemetry.uptime_ms +=
        elapsed_ms;

    BatteryTelemetry battery;

    if (
        battery_monitor_read(&battery)
        != FW_OK
    ) {

        controller->telemetry.fault_flags |=
            0x00000001U;

        return;
    }

    controller->telemetry.battery =
        battery;

    ThermalState thermal =
        thermal_classify(
            controller->telemetry.thermal.cpu_temperature_c,
            controller->telemetry.thermal.gpu_temperature_c,
            controller->telemetry.thermal.board_temperature_c
        );

    controller->telemetry.thermal.state =
        thermal;

    controller->telemetry.power_state =
        power_controller_update(
            &battery,
            thermal
        );

    controller->tick_count++;
}

const FirmwareTelemetry *
controller_telemetry(
    const SurfaceController *controller
)
{
    if (controller == 0) {
        return 0;
    }

    return &controller->telemetry;
}
14. Firmware main loop
main/main.c
#include "controller.h"
#include "scheduler.h"
#include "watchdog.h"
#include "firmware_config.h"

static SurfaceController controller;
static Scheduler scheduler;
static FirmwareWatchdog watchdog;

static void controller_task(void)
{
    controller_tick(
        &controller,
        FW_TICK_MS
    );

    watchdog_kick(&watchdog);
}

int main(void)
{
    controller_init(&controller);

    scheduler_init(&scheduler);

    watchdog_init(
        &watchdog,
        FW_WATCHDOG_TIMEOUT_MS
    );

    scheduler_add(
        &scheduler,
        controller_task,
        FW_TICK_MS
    );

    /*
     * Real MCU startup would initialise clocks,
     * interrupts, GPIO, ADC, PWM, I2C/SPI,
     * hardware watchdog and communication peripherals.
     */

    for (;;) {

        scheduler_tick(
            &scheduler,
            FW_TICK_MS
        );

        watchdog_tick(
            &watchdog,
            FW_TICK_MS
        );

        if (watchdog_expired(&watchdog)) {

            /*
             * Production implementation:
             * deliberately trigger the MCU
             * hardware watchdog reset.
             */

            break;
        }
    }

    return 0;
}
15. Hardware abstraction layer

The important architectural separation is:

                    Embedded C
                       │
          ┌────────────┴────────────┐
          │                         │
     Deterministic Core        Hardware HAL
          │                         │
     power_controller        GPIO / ADC / PWM
     thermal_controller      I2C / SPI / UART
     scheduler               watchdog / timers
          │                         │
          └────────────┬────────────┘
                       ▼
                 Specific MCU

This means the same control logic can be tested on a PC without pretending that a PC is the Surface motherboard.

16. Simulated HAL
drivers/src/simulated_hal.c
#include "adc.h"
#include "gpio.h"
#include "pwm.h"

#include <stddef.h>

static uint16_t simulated_adc[8];

void adc_init(void)
{
    for (int i = 0; i < 8; ++i) {
        simulated_adc[i] = 0U;
    }

    simulated_adc[0] = 4000U;
    simulated_adc[1] = 12000U;
}

uint16_t adc_read(
    uint8_t channel
)
{
    if (channel >= 8U) {
        return 0U;
    }

    return simulated_adc[channel];
}

void gpio_init(void)
{
}

bool gpio_read(
    uint32_t pin
)
{
    (void)pin;
    return false;
}

void gpio_write(
    uint32_t pin,
    bool value
)
{
    (void)pin;
    (void)value;
}

void pwm_init(void)
{
}

void pwm_set_fan_rpm(
    uint16_t rpm
)
{
    (void)rpm;
}
17. CMake
CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

project(
    SurfaceEmbeddedFirmware
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(
    SurfaceFirmware
    STATIC

    common/src/crc16.c

    core/src/controller.c
    core/src/scheduler.c
    core/src/watchdog.c
    core/src/ring_buffer.c

    power/src/battery_monitor.c
    power/src/power_controller.c

    thermal/src/thermal_controller.c

    input/src/button_controller.c

    drivers/src/simulated_hal.c
)

target_include_directories(
    SurfaceFirmware
    PUBLIC

    common/include
    core/include
    power/include
    thermal/include
    input/include
    drivers/include
)

add_executable(
    SurfaceFirmwareDemo
    main/main.c
)

target_link_libraries(
    SurfaceFirmwareDemo
    PRIVATE
    SurfaceFirmware
)

enable_testing()

add_executable(
    SurfaceFirmwareTests

    tests/test_crc.c
    tests/test_power.c
    tests/test_thermal.c
    tests/test_scheduler.c
)

target_link_libraries(
    SurfaceFirmwareTests
    PRIVATE
    SurfaceFirmware
)

add_test(
    NAME SurfaceFirmwareTests
    COMMAND SurfaceFirmwareTests
)
18. Thermal tests
tests/test_thermal.c
#include "thermal_controller.h"

#include <assert.h>

int main(void)
{
    assert(
        thermal_classify(50, 50, 40)
        == THERMAL_NORMAL
    );

    assert(
        thermal_classify(75, 60, 50)
        == THERMAL_WARM
    );

    assert(
        thermal_classify(90, 70, 60)
        == THERMAL_HOT
    );

    assert(
        thermal_classify(97, 80, 60)
        == THERMAL_CRITICAL
    );

    assert(
        thermal_classify(101, 90, 70)
        == THERMAL_EMERGENCY
    );

    assert(
        thermal_fan_target(
            THERMAL_CRITICAL
        ) == 5000U
    );

    return 0;
}
19. Power tests
tests/test_power.c
#include "power_controller.h"

#include <assert.h>

int main(void)
{
    BatteryTelemetry battery = {
        .battery_mv = 4000,
        .system_mv = 12000,
        .battery_current_ma = -500,
        .battery_percent = 50,
        .external_power = false
    };

    assert(
        power_controller_update(
            &battery,
            THERMAL_NORMAL
        ) == POWER_STATE_ACTIVE
    );

    battery.battery_percent = 4;

    assert(
        power_controller_update(
            &battery,
            THERMAL_NORMAL
        ) == POWER_STATE_OFF
    );

    battery.battery_percent = 50;

    assert(
        power_controller_update(
            &battery,
            THERMAL_CRITICAL
        ) == POWER_STATE_FAULT
    );

    return 0;
}






27 — Surface USB / Thunderbolt Device Stack
Project structure
SurfaceUSBThunderbolt/
├── CMakeLists.txt
├── README.md
│
├── cpp/
│   ├── include/
│   │   ├── USBTypes.hpp
│   │   ├── USBDevice.hpp
│   │   ├── USBInterface.hpp
│   │   ├── USBEndpoint.hpp
│   │   ├── USBTransfer.hpp
│   │   ├── USBTopology.hpp
│   │   ├── USBEnumerator.hpp
│   │   ├── USBHotplugMonitor.hpp
│   │   ├── USBTransferEngine.hpp
│   │   ├── ThunderboltDevice.hpp
│   │   ├── ThunderboltTopology.hpp
│   │   ├── DevicePolicy.hpp
│   │   └── USBThunderboltEngine.hpp
│   │
│   └── src/
│       ├── USBDevice.cpp
│       ├── USBInterface.cpp
│       ├── USBEndpoint.cpp
│       ├── USBTransfer.cpp
│       ├── USBTopology.cpp
│       ├── USBEnumerator.cpp
│       ├── USBHotplugMonitor.cpp
│       ├── USBTransferEngine.cpp
│       ├── ThunderboltDevice.cpp
│       ├── ThunderboltTopology.cpp
│       ├── DevicePolicy.cpp
│       ├── USBThunderboltEngine.cpp
│       └── main.cpp
│
├── rust/
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── types.rs
│       ├── policy.rs
│       ├── security.rs
│       ├── topology.rs
│       ├── transfer.rs
│       ├── rate_limiter.rs
│       └── ffi.rs
│
└── tests/
    ├── USBTypesTests.cpp
    ├── USBTopologyTests.cpp
    ├── USBTransferTests.cpp
    ├── DevicePolicyTests.cpp
    └── rust_policy_tests.rs
1. USBTypes.hpp

This is the common vocabulary used by the entire C++ layer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace surface::usb {

enum class BusType : uint8_t {
    USB2,
    USB3,
    USB4,
    Thunderbolt3,
    Thunderbolt4,
    Thunderbolt5,
    Unknown
};

enum class DeviceState : uint8_t {
    Disconnected,
    Enumerating,
    Ready,
    Suspended,
    Error
};

enum class Direction : uint8_t {
    HostToDevice,
    DeviceToHost
};

enum class EndpointType : uint8_t {
    Control,
    Bulk,
    Interrupt,
    Isochronous
};

enum class TransferStatus : uint8_t {
    Pending,
    Completed,
    Cancelled,
    Timeout,
    Stall,
    DeviceRemoved,
    Error
};

enum class SecurityLevel : uint8_t {
    Unknown,
    Untrusted,
    Restricted,
    Trusted
};

struct DeviceId {
    uint16_t vendorId{};
    uint16_t productId{};
    uint16_t bcdDevice{};

    std::string serialNumber;
};

struct USBEndpointDescriptor {
    uint8_t address{};
    EndpointType type{EndpointType::Control};

    Direction direction{Direction::HostToDevice};

    uint16_t maxPacketSize{};
    uint8_t interval{};

    bool isInput() const {
        return direction == Direction::DeviceToHost;
    }
};

struct USBInterfaceDescriptor {
    uint8_t interfaceNumber{};
    uint8_t alternateSetting{};

    uint8_t interfaceClass{};
    uint8_t interfaceSubClass{};
    uint8_t interfaceProtocol{};

    std::vector<USBEndpointDescriptor> endpoints;
};

struct USBDeviceInfo {
    DeviceId id;

    BusType busType{BusType::Unknown};
    DeviceState state{DeviceState::Disconnected};

    std::wstring instanceId;
    std::wstring devicePath;

    std::wstring manufacturer;
    std::wstring product;

    std::vector<USBInterfaceDescriptor> interfaces;

    SecurityLevel security{SecurityLevel::Unknown};

    uint64_t rxBytes{};
    uint64_t txBytes{};
};

struct TransferRequest {
    uint8_t endpoint{};
    Direction direction{Direction::HostToDevice};

    std::vector<uint8_t> data;

    uint32_t timeoutMs{5000};
};

struct TransferResult {
    TransferStatus status{TransferStatus::Error};

    std::vector<uint8_t> data;

    uint32_t transferredBytes{};
    uint32_t errorCode{};
};

}
2. USB device abstraction
USBDevice.hpp
#pragma once

#include "USBTypes.hpp"

#include <mutex>
#include <functional>

namespace surface::usb {

class USBDevice {
public:
    using TransferCallback =
        std::function<void(const TransferResult&)>;

    explicit USBDevice(USBDeviceInfo info);

    const USBDeviceInfo& info() const noexcept;

    bool open();
    void close();

    bool isOpen() const noexcept;

    TransferResult controlTransfer(
        const TransferRequest& request);

    TransferResult bulkTransfer(
        const TransferRequest& request);

    void bulkTransferAsync(
        TransferRequest request,
        TransferCallback callback);

private:
    USBDeviceInfo info_;

    mutable std::mutex mutex_;

    bool open_{false};
};

}
USBDevice.cpp
#include "USBDevice.hpp"

namespace surface::usb {

USBDevice::USBDevice(USBDeviceInfo info)
    : info_(std::move(info)) {
}

const USBDeviceInfo& USBDevice::info() const noexcept {
    return info_;
}

bool USBDevice::open() {
    std::lock_guard lock(mutex_);

    if (open_) {
        return true;
    }

    /*
     * Production implementation:
     *
     * - obtain Windows device interface path
     * - open with CreateFileW
     * - initialize WinUSB where appropriate
     * - query descriptors
     * - establish interface/alternate setting
     *
     * Do not bypass the Windows USB driver model.
     */

    open_ = true;
    return true;
}

void USBDevice::close() {
    std::lock_guard lock(mutex_);

    if (!open_) {
        return;
    }

    /*
     * Production:
     * WinUsb_Free()
     * CloseHandle()
     */

    open_ = false;
}

bool USBDevice::isOpen() const noexcept {
    std::lock_guard lock(mutex_);
    return open_;
}

TransferResult USBDevice::controlTransfer(
    const TransferRequest& request) {

    std::lock_guard lock(mutex_);

    if (!open_) {
        return {
            TransferStatus::Error,
            {},
            0,
            1
        };
    }

    /*
     * Production:
     * WINUSB_SETUP_PACKET
     * WinUsb_ControlTransfer()
     */

    return {
        TransferStatus::Completed,
        request.data,
        static_cast<uint32_t>(request.data.size()),
        0
    };
}

TransferResult USBDevice::bulkTransfer(
    const TransferRequest& request) {

    std::lock_guard lock(mutex_);

    if (!open_) {
        return {
            TransferStatus::Error,
            {},
            0,
            1
        };
    }

    /*
     * Production:
     * WinUsb_ReadPipe()
     * WinUsb_WritePipe()
     */

    return {
        TransferStatus::Completed,
        request.data,
        static_cast<uint32_t>(request.data.size()),
        0
    };
}

void USBDevice::bulkTransferAsync(
    TransferRequest request,
    TransferCallback callback) {

    /*
     * Production implementation should use an asynchronous
     * transfer queue with OVERLAPPED I/O / WinUSB asynchronous
     * operations rather than spawning an unbounded thread per
     * transfer.
     */

    TransferResult result = bulkTransfer(request);

    if (callback) {
        callback(result);
    }
}

}
3. Endpoint management
USBEndpoint.hpp
#pragma once

#include "USBTypes.hpp"

#include <atomic>

namespace surface::usb {

class USBEndpoint {
public:
    explicit USBEndpoint(USBEndpointDescriptor descriptor);

    const USBEndpointDescriptor& descriptor() const noexcept;

    void recordTransfer(uint32_t bytes);

    uint64_t transferredBytes() const noexcept;

private:
    USBEndpointDescriptor descriptor_;

    std::atomic<uint64_t> transferredBytes_{0};
};

}
USBEndpoint.cpp
#include "USBEndpoint.hpp"

namespace surface::usb {

USBEndpoint::USBEndpoint(
    USBEndpointDescriptor descriptor)
    : descriptor_(descriptor) {
}

const USBEndpointDescriptor&
USBEndpoint::descriptor() const noexcept {
    return descriptor_;
}

void USBEndpoint::recordTransfer(uint32_t bytes) {
    transferredBytes_.fetch_add(
        bytes,
        std::memory_order_relaxed);
}

uint64_t USBEndpoint::transferredBytes() const noexcept {
    return transferredBytes_.load(
        std::memory_order_relaxed);
}

}
4. Device enumeration
USBEnumerator.hpp
#pragma once

#include "USBTypes.hpp"

#include <vector>

namespace surface::usb {

class USBEnumerator {
public:
    std::vector<USBDeviceInfo> enumerate();

private:
    std::vector<USBDeviceInfo>
    enumerateWindowsUSB();

    BusType classifyBusType(
        const USBDeviceInfo& device) const;
};

}
USBEnumerator.cpp
#include "USBEnumerator.hpp"

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#endif

namespace surface::usb {

std::vector<USBDeviceInfo>
USBEnumerator::enumerate() {

#ifdef _WIN32

    /*
     * Production implementation:
     *
     * SetupDiGetClassDevs()
     * SetupDiEnumDeviceInterfaces()
     * SetupDiGetDeviceInterfaceDetail()
     *
     * followed by descriptor interrogation through the
     * appropriate supported Windows USB interface.
     */

    return enumerateWindowsUSB();

#else

    return {};

#endif
}

std::vector<USBDeviceInfo>
USBEnumerator::enumerateWindowsUSB() {

    std::vector<USBDeviceInfo> devices;

#ifdef _WIN32

    /*
     * The actual GUID/interface enumeration is intentionally
     * kept behind this adapter.
     *
     * This prevents Windows-specific discovery code from
     * contaminating the rest of the USB architecture.
     */

#endif

    return devices;
}

BusType USBEnumerator::classifyBusType(
    const USBDeviceInfo& device) const {

    return device.busType;
}

}

This is deliberate: don't invent undocumented Surface USB APIs.

5. USB topology
USBTopology.hpp
#pragma once

#include "USBTypes.hpp"

#include <unordered_map>
#include <shared_mutex>

namespace surface::usb {

class USBTopology {
public:
    void addDevice(const USBDeviceInfo& device);

    void removeDevice(
        const std::wstring& instanceId);

    std::optional<USBDeviceInfo> find(
        const std::wstring& instanceId) const;

    std::vector<USBDeviceInfo> devices() const;

    size_t size() const;

private:
    std::unordered_map<
        std::wstring,
        USBDeviceInfo
    > devices_;

    mutable std::shared_mutex mutex_;
};

}
USBTopology.cpp
#include "USBTopology.hpp"

namespace surface::usb {

void USBTopology::addDevice(
    const USBDeviceInfo& device) {

    std::unique_lock lock(mutex_);

    devices_[device.instanceId] = device;
}

void USBTopology::removeDevice(
    const std::wstring& instanceId) {

    std::unique_lock lock(mutex_);

    devices_.erase(instanceId);
}

std::optional<USBDeviceInfo>
USBTopology::find(
    const std::wstring& instanceId) const {

    std::shared_lock lock(mutex_);

    auto it = devices_.find(instanceId);

    if (it == devices_.end()) {
        return std::nullopt;
    }

    return it->second;
}

std::vector<USBDeviceInfo>
USBTopology::devices() const {

    std::shared_lock lock(mutex_);

    std::vector<USBDeviceInfo> result;

    result.reserve(devices_.size());

    for (const auto& [_, device] : devices_) {
        result.push_back(device);
    }

    return result;
}

size_t USBTopology::size() const {

    std::shared_lock lock(mutex_);

    return devices_.size();
}

}
6. Hot-plug monitoring

This is particularly important for Surface because users constantly attach/remove:

USB-C docks
displays
storage
keyboards
cameras
audio interfaces
Ethernet adapters
Thunderbolt devices
USBHotplugMonitor.hpp
#pragma once

#include <functional>
#include <string>

namespace surface::usb {

class USBHotplugMonitor {
public:
    using DeviceCallback =
        std::function<void(const std::wstring& instanceId)>;

    bool start(
        DeviceCallback arrival,
        DeviceCallback removal);

    void stop();

private:
    bool running_{false};

    DeviceCallback arrival_;
    DeviceCallback removal_;
};

}
USBHotplugMonitor.cpp
#include "USBHotplugMonitor.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace surface::usb {

bool USBHotplugMonitor::start(
    DeviceCallback arrival,
    DeviceCallback removal) {

    if (running_) {
        return true;
    }

    arrival_ = std::move(arrival);
    removal_ = std::move(removal);

    /*
     * Production Windows implementation:
     *
     * RegisterDeviceNotificationW()
     *
     * attached to the service/window/device-management
     * event loop.
     */

    running_ = true;

    return true;
}

void USBHotplugMonitor::stop() {

    if (!running_) {
        return;
    }

    /*
     * Production:
     * UnregisterDeviceNotification()
     */

    running_ = false;
}

}
7. Transfer engine
USBTransferEngine.hpp
#pragma once

#include "USBDevice.hpp"

#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>

namespace surface::usb {

struct QueuedTransfer {
    std::shared_ptr<USBDevice> device;
    TransferRequest request;
    USBDevice::TransferCallback callback;
};

class USBTransferEngine {
public:
    USBTransferEngine();

    ~USBTransferEngine();

    void start();

    void stop();

    bool submit(QueuedTransfer transfer);

    size_t pending() const;

private:
    void worker();

    std::queue<QueuedTransfer> queue_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::thread worker_;

    std::atomic<bool> running_{false};
};

}
USBTransferEngine.cpp
#include "USBTransferEngine.hpp"

namespace surface::usb {

USBTransferEngine::USBTransferEngine() = default;

USBTransferEngine::~USBTransferEngine() {
    stop();
}

void USBTransferEngine::start() {

    bool expected = false;

    if (!running_.compare_exchange_strong(
            expected,
            true)) {
        return;
    }

    worker_ = std::thread(
        &USBTransferEngine::worker,
        this);
}

void USBTransferEngine::stop() {

    if (!running_.exchange(false)) {
        return;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

bool USBTransferEngine::submit(
    QueuedTransfer transfer) {

    if (!running_) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);

        queue_.push(std::move(transfer));
    }

    cv_.notify_one();

    return true;
}

size_t USBTransferEngine::pending() const {

    std::lock_guard lock(mutex_);

    return queue_.size();
}

void USBTransferEngine::worker() {

    while (running_) {

        QueuedTransfer transfer;

        {
            std::unique_lock lock(mutex_);

            cv_.wait(
                lock,
                [this] {
                    return !running_ ||
                           !queue_.empty();
                });

            if (!running_ && queue_.empty()) {
                break;
            }

            transfer = std::move(queue_.front());
            queue_.pop();
        }

        if (!transfer.device) {
            continue;
        }

        auto result =
            transfer.device->bulkTransfer(
                transfer.request);

        if (transfer.callback) {
            transfer.callback(result);
        }
    }
}

}

For a production implementation, this becomes considerably more sophisticated: bounded queues, overlapped I/O, cancellation, per-device concurrency limits, transfer deadlines and device-removal cancellation.

8. Thunderbolt device model

Thunderbolt/USB4 needs to be represented separately because it isn't simply another USB endpoint.

ThunderboltDevice.hpp
#pragma once

#include "USBTypes.hpp"

#include <string>
#include <vector>

namespace surface::usb {

struct ThunderboltDeviceInfo {
    std::wstring instanceId;

    std::wstring vendor;
    std::wstring deviceName;

    uint64_t routeId{};

    uint32_t domain{};
    uint32_t depth{};

    bool authorized{false};
    bool secure{false};

    BusType transport{BusType::Thunderbolt4};
};

class ThunderboltDevice {
public:
    explicit ThunderboltDevice(
        ThunderboltDeviceInfo info);

    const ThunderboltDeviceInfo& info() const noexcept;

    bool authorize();

    bool deauthorize();

private:
    ThunderboltDeviceInfo info_;
};

}
ThunderboltDevice.cpp
#include "ThunderboltDevice.hpp"

namespace surface::usb {

ThunderboltDevice::ThunderboltDevice(
    ThunderboltDeviceInfo info)
    : info_(std::move(info)) {
}

const ThunderboltDeviceInfo&
ThunderboltDevice::info() const noexcept {
    return info_;
}

bool ThunderboltDevice::authorize() {

    /*
     * Authorization must be performed through the
     * supported Windows/Thunderbolt security mechanisms.
     *
     * Never implement a raw PCIe/Thunderbolt bypass here.
     */

    info_.authorized = true;

    return true;
}

bool ThunderboltDevice::deauthorize() {

    info_.authorized = false;

    return true;
}

}
9. Device security policy

This is where the Rust layer becomes especially valuable.

DevicePolicy.hpp
#pragma once

#include "USBTypes.hpp"

namespace surface::usb {

class DevicePolicy {
public:
    SecurityLevel evaluate(
        const USBDeviceInfo& device) const;

    bool allowBulkTransfer(
        const USBDeviceInfo& device,
        uint32_t byteCount) const;

    bool allowThunderboltDevice(
        const ThunderboltDeviceInfo& device) const;
};

}
DevicePolicy.cpp
#include "DevicePolicy.hpp"
#include "ThunderboltDevice.hpp"

namespace surface::usb {

SecurityLevel DevicePolicy::evaluate(
    const USBDeviceInfo& device) const {

    if (device.id.vendorId == 0 ||
        device.id.productId == 0) {

        return SecurityLevel::Untrusted;
    }

    return device.security;
}

bool DevicePolicy::allowBulkTransfer(
    const USBDeviceInfo& device,
    uint32_t byteCount) const {

    if (byteCount == 0) {
        return false;
    }

    if (byteCount > 16 * 1024 * 1024) {
        return false;
    }

    return evaluate(device) !=
           SecurityLevel::Untrusted;
}

bool DevicePolicy::allowThunderboltDevice(
    const ThunderboltDeviceInfo& device) const {

    return device.secure;
}

}
10. Rust security/policy layer
Cargo.toml
[package]
name = "surface_usb_security"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["rlib", "cdylib"]

[dependencies]
serde = { version = "1", features = ["derive"] }
thiserror = "2"
sha2 = "0.10"
rust/src/types.rs
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[repr(u32)]
pub enum SecurityLevel {
    Unknown = 0,
    Untrusted = 1,
    Restricted = 2,
    Trusted = 3,
}

#[derive(Debug, Clone, Copy)]
pub struct DeviceIdentity {
    pub vendor_id: u16,
    pub product_id: u16,
}

#[derive(Debug, Clone, Copy)]
pub struct TransferPolicy {
    pub allowed: bool,
    pub max_transfer_size: u32,
}
11. Rust USB security policy
rust/src/policy.rs
use crate::types::{
    DeviceIdentity,
    SecurityLevel,
    TransferPolicy,
};

pub fn evaluate_device(
    identity: DeviceIdentity,
) -> SecurityLevel {

    if identity.vendor_id == 0 ||
       identity.product_id == 0 {
        return SecurityLevel::Untrusted;
    }

    SecurityLevel::Restricted
}

pub fn transfer_policy(
    level: SecurityLevel,
    requested: u32,
) -> TransferPolicy {

    let max = match level {
        SecurityLevel::Unknown => 0,
        SecurityLevel::Untrusted => 0,
        SecurityLevel::Restricted => 16 * 1024 * 1024,
        SecurityLevel::Trusted => 64 * 1024 * 1024,
    };

    TransferPolicy {
        allowed: requested > 0 &&
                 requested <= max,
        max_transfer_size: max,
    }
}
12. Rust rate limiter

This prevents a malfunctioning peripheral from generating an uncontrolled transfer storm.

rust/src/rate_limiter.rs
use std::time::{Duration, Instant};

pub struct RateLimiter {
    capacity: u64,
    tokens: u64,
    refill_per_second: u64,
    last: Instant,
}

impl RateLimiter {
    pub fn new(
        capacity: u64,
        refill_per_second: u64,
    ) -> Self {

        Self {
            capacity,
            tokens: capacity,
            refill_per_second,
            last: Instant::now(),
        }
    }

    fn refill(&mut self) {

        let elapsed = self.last.elapsed();

        if elapsed < Duration::from_millis(100) {
            return;
        }

        let seconds =
            elapsed.as_secs();

        if seconds == 0 {
            return;
        }

        let added =
            seconds.saturating_mul(
                self.refill_per_second
            );

        self.tokens =
            self.capacity.min(
                self.tokens.saturating_add(added)
            );

        self.last = Instant::now();
    }

    pub fn allow(&mut self, cost: u64) -> bool {

        self.refill();

        if cost > self.tokens {
            return false;
        }

        self.tokens -= cost;

        true
    }
}
13. Rust FFI boundary
rust/src/ffi.rs
use crate::policy::{
    evaluate_device,
    transfer_policy,
};

use crate::types::DeviceIdentity;

#[repr(C)]
pub struct CUsbPolicy {
    pub security_level: u32,
    pub transfer_allowed: u8,
    pub max_transfer_size: u32,
}

#[no_mangle]
pub extern "C" fn surface_usb_evaluate(
    vendor_id: u16,
    product_id: u16,
    transfer_size: u32,
) -> CUsbPolicy {

    let identity = DeviceIdentity {
        vendor_id,
        product_id,
    };

    let security =
        evaluate_device(identity);

    let policy =
        transfer_policy(
            security,
            transfer_size,
        );

    CUsbPolicy {
        security_level: security as u32,
        transfer_allowed: policy.allowed as u8,
        max_transfer_size:
            policy.max_transfer_size,
    }
}
14. Rust library
rust/src/lib.rs
pub mod types;
pub mod policy;
pub mod rate_limiter;
pub mod ffi;
15. Unified Surface engine
USBThunderboltEngine.hpp
#pragma once

#include "USBEnumerator.hpp"
#include "USBTopology.hpp"
#include "USBHotplugMonitor.hpp"
#include "USBTransferEngine.hpp"
#include "DevicePolicy.hpp"

namespace surface::usb {

class USBThunderboltEngine {
public:
    bool initialize();

    void shutdown();

    std::vector<USBDeviceInfo>
    devices() const;

    bool refresh();

    USBTopology& topology() noexcept;

    USBTransferEngine& transfers() noexcept;

private:
    USBEnumerator enumerator_;
    USBTopology topology_;

    USBHotplugMonitor hotplug_;
    USBTransferEngine transferEngine_;

    DevicePolicy policy_;

    bool initialized_{false};
};

}
USBThunderboltEngine.cpp
#include "USBThunderboltEngine.hpp"

namespace surface::usb {

bool USBThunderboltEngine::initialize() {

    if (initialized_) {
        return true;
    }

    if (!refresh()) {
        return false;
    }

    transferEngine_.start();

    hotplug_.start(
        [this](const std::wstring&) {
            refresh();
        },
        [this](const std::wstring& id) {
            topology_.removeDevice(id);
        }
    );

    initialized_ = true;

    return true;
}

void USBThunderboltEngine::shutdown() {

    if (!initialized_) {
        return;
    }

    hotplug_.stop();
    transferEngine_.stop();

    initialized_ = false;
}

std::vector<USBDeviceInfo>
USBThunderboltEngine::devices() const {

    return topology_.devices();
}

bool USBThunderboltEngine::refresh() {

    auto discovered =
        enumerator_.enumerate();

    for (const auto& device : discovered) {
        topology_.addDevice(device);
    }

    return true;
}

USBTopology&
USBThunderboltEngine::topology() noexcept {
    return topology_;
}

USBTransferEngine&
USBThunderboltEngine::transfers() noexcept {
    return transferEngine_;
}

}
16. CMake
cmake_minimum_required(VERSION 3.25)

project(
    SurfaceUSBThunderbolt
    VERSION 0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
    SurfaceUSBThunderbolt
    STATIC

    cpp/src/USBDevice.cpp
    cpp/src/USBInterface.cpp
    cpp/src/USBEndpoint.cpp
    cpp/src/USBTransfer.cpp
    cpp/src/USBTopology.cpp
    cpp/src/USBEnumerator.cpp
    cpp/src/USBHotplugMonitor.cpp
    cpp/src/USBTransferEngine.cpp
    cpp/src/ThunderboltDevice.cpp
    cpp/src/ThunderboltTopology.cpp
    cpp/src/DevicePolicy.cpp
    cpp/src/USBThunderboltEngine.cpp
)

target_include_directories(
    SurfaceUSBThunderbolt
    PUBLIC
        cpp/include
)

if(WIN32)

    target_link_libraries(
        SurfaceUSBThunderbolt
        PRIVATE
            setupapi
            cfgmgr32
            user32
            advapi32
    )

endif()

add_executable(
    SurfaceUSBThunderboltDemo
    cpp/src/main.cpp
)

target_link_libraries(
    SurfaceUSBThunderboltDemo
    PRIVATE
        SurfaceUSBThunderbolt
)

enable_testing()

add_executable(
    USBTests
    tests/USBTypesTests.cpp
    tests/USBTopologyTests.cpp
    tests/USBTransferTests.cpp
    tests/DevicePolicyTests.cpp
)

target_link_libraries(
    USBTests
    PRIVATE
        SurfaceUSBThunderbolt
)

add_test(
    NAME USBTests
    COMMAND USBTests
)

The CMake list assumes the small interface/transfer/thunderbolt source files exist; if keeping the implementation minimal, those can be empty translation units initially.

17. Basic tests
USBTopologyTests.cpp
#include "USBTopology.hpp"

#include <cassert>

using namespace surface::usb;

int main() {

    USBTopology topology;

    USBDeviceInfo device;

    device.instanceId =
        L"USB\\VID_1234&PID_5678";

    device.id.vendorId = 0x1234;
    device.id.productId = 0x5678;

    device.state =
        DeviceState::Ready;

    topology.addDevice(device);

    assert(topology.size() == 1);

    auto found =
        topology.find(device.instanceId);

    assert(found.has_value());

    assert(
        found->id.vendorId == 0x1234
    );

    topology.removeDevice(
        device.instanceId
    );

    assert(topology.size() == 0);

    return 0;
}
18. Device policy tests
#include "DevicePolicy.hpp"

#include <cassert>

using namespace surface::usb;

int main() {

    DevicePolicy policy;

    USBDeviceInfo device;

    device.id.vendorId = 0x1234;
    device.id.productId = 0x5678;

    device.security =
        SecurityLevel::Trusted;

    assert(
        policy.allowBulkTransfer(
            device,
            4096
        )
    );

    assert(
        !policy.allowBulkTransfer(
            device,
            0
        )
    );

    assert(
        !policy.allowBulkTransfer(
            device,
            32 * 1024 * 1024
        )
    );

    return 0;
}
19. main.cpp
#include "USBThunderboltEngine.hpp"

#include <iostream>

int main() {

    using namespace surface::usb;

    USBThunderboltEngine engine;

    if (!engine.initialize()) {

        std::cerr
            << "Failed to initialize USB stack\n";

        return 1;
    }

    auto devices =
        engine.devices();

    std::cout
        << "Surface USB/Thunderbolt stack\n";

    std::cout
        << "Devices: "
        << devices.size()
        << '\n';

    for (const auto& device : devices) {

        std::wcout
            << L"Device: "
            << device.product
            << L"\n";

        std::wcout
            << L"Instance: "
            << device.instanceId
            << L"\n";
    }

    engine.shutdown();

    return 0;
}






Project structure
SurfaceLocalIndex/
├── CMakeLists.txt
├── README.md
│
├── cpp/
│   ├── include/
│   │   ├── IndexTypes.hpp
│   │   ├── Document.hpp
│   │   ├── Tokenizer.hpp
│   │   ├── InvertedIndex.hpp
│   │   ├── QueryEngine.hpp
│   │   ├── Segment.hpp
│   │   ├── SegmentManager.hpp
│   │   ├── IndexCache.hpp
│   │   ├── StorageEngine.hpp
│   │   ├── IndexScheduler.hpp
│   │   └── SurfaceIndexEngine.hpp
│   │
│   └── src/
│       ├── Document.cpp
│       ├── Tokenizer.cpp
│       ├── InvertedIndex.cpp
│       ├── QueryEngine.cpp
│       ├── Segment.cpp
│       ├── SegmentManager.cpp
│       ├── IndexCache.cpp
│       ├── StorageEngine.cpp
│       ├── IndexScheduler.cpp
│       ├── SurfaceIndexEngine.cpp
│       └── main.cpp
│
├── rust/
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── types.rs
│       ├── journal.rs
│       ├── checksum.rs
│       ├── segment.rs
│       ├── storage.rs
│       ├── compaction.rs
│       ├── recovery.rs
│       └── ffi.rs
│
└── tests/
    ├── TokenizerTests.cpp
    ├── IndexTests.cpp
    ├── QueryTests.cpp
    ├── StorageTests.cpp
    └── rust_storage_tests.rs
1. Core data model
IndexTypes.hpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace surface::index {

using DocumentId = uint64_t;
using TermId = uint32_t;

struct DocumentMetadata {
    DocumentId id{};
    uint64_t modifiedTime{};
    uint64_t size{};
    std::wstring path;
};

struct SearchHit {
    DocumentId documentId{};
    float score{};
    std::wstring path;
};

struct IndexStatistics {
    uint64_t documents{};
    uint64_t terms{};
    uint64_t postings{};
    uint64_t indexedBytes{};
};

enum class DocumentState : uint8_t {
    New,
    Indexed,
    Modified,
    Deleted,
    Error
};

}
2. Documents
Document.hpp
#pragma once

#include "IndexTypes.hpp"

#include <string>

namespace surface::index {

struct Document {
    DocumentMetadata metadata;

    std::wstring title;
    std::wstring content;

    DocumentState state{
        DocumentState::New
    };
};

}
3. Fast tokenizer

The tokenizer deliberately stays separate from the index.

That means it can later be replaced with a SIMD/Unicode-aware implementation without rewriting the storage engine.

Tokenizer.hpp
#pragma once

#include <string>
#include <vector>

namespace surface::index {

class Tokenizer {
public:
    std::vector<std::wstring>
    tokenize(const std::wstring& text) const;

private:
    bool isWordCharacter(wchar_t c) const;
};

}
Tokenizer.cpp
#include "Tokenizer.hpp"

#include <cwctype>

namespace surface::index {

bool Tokenizer::isWordCharacter(
    wchar_t c) const {

    return std::iswalnum(c) || c == L'_';
}

std::vector<std::wstring>
Tokenizer::tokenize(
    const std::wstring& text) const {

    std::vector<std::wstring> tokens;

    std::wstring current;

    for (wchar_t c : text) {

        if (isWordCharacter(c)) {

            current.push_back(
                static_cast<wchar_t>(
                    std::towlower(c)
                )
            );

        } else if (!current.empty()) {

            tokens.push_back(
                std::move(current)
            );

            current.clear();
        }
    }

    if (!current.empty()) {
        tokens.push_back(
            std::move(current)
        );
    }

    return tokens;
}

}

For production, this should eventually become Unicode-normalized, language-aware and SIMD optimized.

4. Inverted index

The core search structure is:

term
 │
 ├── document 10
 ├── document 42
 ├── document 91
 └── document 133
InvertedIndex.hpp
#pragma once

#include "IndexTypes.hpp"

#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>

namespace surface::index {

class InvertedIndex {
public:
    void add(
        TermId term,
        DocumentId document);

    void remove(
        TermId term,
        DocumentId document);

    std::vector<DocumentId>
    lookup(TermId term) const;

    size_t termCount() const;

    size_t postingCount() const;

private:
    std::unordered_map<
        TermId,
        std::unordered_set<DocumentId>
    > postings_;

    mutable std::shared_mutex mutex_;
};

}
InvertedIndex.cpp
#include "InvertedIndex.hpp"

namespace surface::index {

void InvertedIndex::add(
    TermId term,
    DocumentId document) {

    std::unique_lock lock(mutex_);

    postings_[term].insert(document);
}

void InvertedIndex::remove(
    TermId term,
    DocumentId document) {

    std::unique_lock lock(mutex_);

    auto it = postings_.find(term);

    if (it == postings_.end()) {
        return;
    }

    it->second.erase(document);

    if (it->second.empty()) {
        postings_.erase(it);
    }
}

std::vector<DocumentId>
InvertedIndex::lookup(TermId term) const {

    std::shared_lock lock(mutex_);

    auto it = postings_.find(term);

    if (it == postings_.end()) {
        return {};
    }

    return {
        it->second.begin(),
        it->second.end()
    };
}

size_t InvertedIndex::termCount() const {

    std::shared_lock lock(mutex_);

    return postings_.size();
}

size_t InvertedIndex::postingCount() const {

    std::shared_lock lock(mutex_);

    size_t count = 0;

    for (const auto& [_, postings] : postings_) {
        count += postings.size();
    }

    return count;
}

}

This is intentionally the simple reference implementation. The production version should use sorted postings and compressed integer arrays rather than unordered_set.

5. Query engine
QueryEngine.hpp
#pragma once

#include "InvertedIndex.hpp"
#include "Tokenizer.hpp"

#include <unordered_map>

namespace surface::index {

class QueryEngine {
public:
    QueryEngine(
        const InvertedIndex& index,
        const Tokenizer& tokenizer);

    std::vector<SearchHit>
    search(
        const std::wstring& query,
        size_t limit = 50) const;

private:
    const InvertedIndex& index_;
    const Tokenizer& tokenizer_;
};

}
QueryEngine.cpp
#include "QueryEngine.hpp"

#include <algorithm>
#include <unordered_map>

namespace surface::index {

QueryEngine::QueryEngine(
    const InvertedIndex& index,
    const Tokenizer& tokenizer)
    : index_(index),
      tokenizer_(tokenizer) {
}

std::vector<SearchHit>
QueryEngine::search(
    const std::wstring& query,
    size_t limit) const {

    auto terms =
        tokenizer_.tokenize(query);

    if (terms.empty()) {
        return {};
    }

    std::unordered_map<
        DocumentId,
        uint32_t
    > scores;

    /*
     * Reference implementation:
     * each matching term contributes one point.
     *
     * Production implementation:
     * BM25 / TF-IDF / field weighting / phrase
     * matching / positional postings.
     */

    for (const auto& term : terms) {

        /*
         * A real implementation needs a term dictionary
         * mapping strings -> TermId.
         *
         * This reference engine therefore leaves that
         * mapping behind the storage/index layer.
         */
        (void)term;
    }

    std::vector<SearchHit> results;

    results.reserve(
        std::min(limit, scores.size())
    );

    for (const auto& [document, score] : scores) {

        results.push_back({
            document,
            static_cast<float>(score),
            {}
        });
    }

    std::sort(
        results.begin(),
        results.end(),
        [](const auto& a, const auto& b) {
            return a.score > b.score;
        }
    );

    if (results.size() > limit) {
        results.resize(limit);
    }

    return results;
}

}

The important production change here is to add a term dictionary.

6. Segment architecture

Instead of keeping the entire index in RAM:

                 Index
                   │
        ┌──────────┼──────────┐
        ▼          ▼          ▼
    Segment 0   Segment 1   Segment 2
        │          │          │
       SSD        SSD        SSD

This is much more appropriate for large local datasets.

Segment.hpp
#pragma once

#include "IndexTypes.hpp"

#include <string>
#include <vector>

namespace surface::index {

struct Posting {
    DocumentId document{};
};

class Segment {
public:
    explicit Segment(uint64_t id);

    uint64_t id() const noexcept;

    void addPosting(
        TermId term,
        DocumentId document);

    const std::vector<Posting>&
    postings(TermId term) const;

    size_t postingCount() const noexcept;

private:
    uint64_t id_;

    std::unordered_map<
        TermId,
        std::vector<Posting>
    > postings_;

    size_t postingCount_{};
};

}
Segment.cpp
#include "Segment.hpp"

namespace surface::index {

Segment::Segment(uint64_t id)
    : id_(id) {
}

uint64_t Segment::id() const noexcept {
    return id_;
}

void Segment::addPosting(
    TermId term,
    DocumentId document) {

    postings_[term].push_back({
        document
    });

    ++postingCount_;
}

const std::vector<Posting>&
Segment::postings(TermId term) const {

    static const std::vector<Posting> empty;

    auto it = postings_.find(term);

    if (it == postings_.end()) {
        return empty;
    }

    return it->second;
}

size_t Segment::postingCount() const noexcept {
    return postingCount_;
}

}

Add:

#include <unordered_map>

to the header.

7. Segment manager
SegmentManager.hpp
#pragma once

#include "Segment.hpp"

#include <memory>
#include <shared_mutex>
#include <vector>

namespace surface::index {

class SegmentManager {
public:
    uint64_t createSegment();

    void add(
        TermId term,
        DocumentId document);

    std::vector<DocumentId>
    lookup(TermId term) const;

    size_t segmentCount() const;

private:
    std::vector<
        std::unique_ptr<Segment>
    > segments_;

    uint64_t nextSegmentId_{1};

    mutable std::shared_mutex mutex_;
};

}
SegmentManager.cpp
#include "SegmentManager.hpp"

#include <unordered_set>

namespace surface::index {

uint64_t SegmentManager::createSegment() {

    std::unique_lock lock(mutex_);

    const uint64_t id =
        nextSegmentId_++;

    segments_.push_back(
        std::make_unique<Segment>(id)
    );

    return id;
}

void SegmentManager::add(
    TermId term,
    DocumentId document) {

    std::unique_lock lock(mutex_);

    if (segments_.empty()) {
        segments_.push_back(
            std::make_unique<Segment>(
                nextSegmentId_++
            )
        );
    }

    segments_.back()->addPosting(
        term,
        document
    );
}

std::vector<DocumentId>
SegmentManager::lookup(
    TermId term) const {

    std::shared_lock lock(mutex_);

    std::unordered_set<DocumentId> result;

    for (const auto& segment : segments_) {

        for (const auto& posting :
             segment->postings(term)) {

            result.insert(
                posting.document
            );
        }
    }

    return {
        result.begin(),
        result.end()
    };
}

size_t SegmentManager::segmentCount() const {

    std::shared_lock lock(mutex_);

    return segments_.size();
}

}
8. Rust crash-safe journal

This is where Rust earns its place.

Cargo.toml
[package]
name = "surface_local_storage"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["rlib", "cdylib"]

[dependencies]
sha2 = "0.10"
thiserror = "2"
types.rs
#[derive(Debug, Clone, Copy)]
#[repr(u8)]
pub enum RecordType {
    Add = 1,
    Delete = 2,
    Update = 3,
}

#[derive(Debug, Clone, Copy)]
pub struct JournalHeader {
    pub version: u16,
    pub record_type: RecordType,
    pub sequence: u64,
    pub payload_length: u32,
}
9. Rust checksum
checksum.rs
use sha2::{Digest, Sha256};

pub fn checksum(data: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();

    hasher.update(data);

    let result = hasher.finalize();

    result.into()
}
10. Rust write-ahead journal
journal.rs
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::Path;

use crate::checksum::checksum;

pub struct Journal {
    file: File,
    sequence: u64,
}

impl Journal {

    pub fn open(
        path: impl AsRef<Path>
    ) -> io::Result<Self> {

        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .read(true)
            .open(path)?;

        Ok(Self {
            file,
            sequence: 0,
        })
    }

    pub fn append(
        &mut self,
        payload: &[u8]
    ) -> io::Result<u64> {

        self.sequence =
            self.sequence.wrapping_add(1);

        let checksum =
            checksum(payload);

        self.file.write_all(
            &self.sequence.to_le_bytes()
        )?;

        self.file.write_all(
            &(payload.len() as u32)
                .to_le_bytes()
        )?;

        self.file.write_all(payload)?;

        self.file.write_all(&checksum)?;

        self.file.sync_data()?;

        Ok(self.sequence)
    }
}

This creates a simple:

[sequence]
[length]
[payload]
[SHA-256]

record.

For a production implementation, the journal needs explicit versioning, framing, corruption recovery and checksummed headers too.

11. Rust recovery
recovery.rs
use std::fs::File;
use std::io::{self, Read};
use std::path::Path;

use crate::checksum::checksum;

pub struct RecoveredRecord {
    pub sequence: u64,
    pub payload: Vec<u8>,
}

pub fn recover(
    path: impl AsRef<Path>
) -> io::Result<Vec<RecoveredRecord>> {

    let mut file = File::open(path)?;

    let mut result = Vec::new();

    loop {

        let mut sequence_bytes = [0u8; 8];

        match file.read_exact(
            &mut sequence_bytes
        ) {
            Ok(_) => {}
            Err(e)
                if e.kind() ==
                   io::ErrorKind::UnexpectedEof =>
            {
                break;
            }
            Err(e) => return Err(e),
        }

        let sequence =
            u64::from_le_bytes(
                sequence_bytes
            );

        let mut length_bytes = [0u8; 4];

        file.read_exact(
            &mut length_bytes
        )?;

        let length =
            u32::from_le_bytes(
                length_bytes
            ) as usize;

        if length > 256 * 1024 * 1024 {
            return Err(
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "record too large"
                )
            );
        }

        let mut payload =
            vec![0u8; length];

        file.read_exact(
            &mut payload
        )?;

        let mut expected = [0u8; 32];

        file.read_exact(
            &mut expected
        )?;

        if checksum(&payload) != expected {
            return Err(
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "checksum mismatch"
                )
            );
        }

        result.push(
            RecoveredRecord {
                sequence,
                payload,
            }
        );
    }

    Ok(result)
}

This gives the indexing layer a proper recovery primitive after an unexpected shutdown.

12. Rust compaction

As the index grows:

Segment 1 ─┐
Segment 2 ─┼──► COMPACTION ──► Segment 8
Segment 3 ─┤
Segment 4 ─┘
compaction.rs
#[derive(Debug, Clone, Copy)]
pub struct CompactionPolicy {
    pub max_segments: usize,
    pub max_deleted_ratio: f32,
}

impl Default for CompactionPolicy {
    fn default() -> Self {
        Self {
            max_segments: 16,
            max_deleted_ratio: 0.25,
        }
    }
}

pub fn should_compact(
    segments: usize,
    deleted_ratio: f32,
    policy: CompactionPolicy,
) -> bool {

    segments >= policy.max_segments ||
        deleted_ratio >=
        policy.max_deleted_ratio
}
13. Rust FFI
ffi.rs
use crate::compaction::{
    should_compact,
    CompactionPolicy,
};

#[repr(C)]
pub struct CCompactionDecision {
    pub compact: u8,
}

#[no_mangle]
pub extern "C" fn
surface_index_should_compact(
    segments: usize,
    deleted_ratio: f32,
) -> CCompactionDecision {

    let policy =
        CompactionPolicy::default();

    CCompactionDecision {
        compact: should_compact(
            segments,
            deleted_ratio,
            policy
        ) as u8,
    }
}
14. Rust lib.rs
pub mod types;
pub mod checksum;
pub mod journal;
pub mod recovery;
pub mod compaction;
pub mod ffi;
15. Cache

A local index should not repeatedly hit NVMe for the same metadata.

IndexCache.hpp
#pragma once

#include "Document.hpp"

#include <list>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace surface::index {

class IndexCache {
public:
    explicit IndexCache(
        size_t capacity = 4096);

    void put(Document document);

    std::optional<Document>
    get(DocumentId id);

    void erase(DocumentId id);

private:
    struct Entry {
        Document document;
    };

    size_t capacity_;

    std::list<DocumentId> lru_;

    std::unordered_map<
        DocumentId,
        std::pair<
            Entry,
            std::list<DocumentId>::iterator
        >
    > entries_;

    std::mutex mutex_;
};

}
16. Storage engine
#pragma once

#include "Document.hpp"

#include <filesystem>
#include <optional>

namespace surface::index {

class StorageEngine {
public:
    explicit StorageEngine(
        std::filesystem::path root);

    bool initialize();

    bool write(
        const Document& document);

    std::optional<Document>
    read(DocumentId id) const;

    bool remove(DocumentId id);

private:
    std::filesystem::path root_;
};

}

The production version should use a binary record format, rather than serializing documents as ad-hoc text.

17. Unified engine
SurfaceIndexEngine.hpp
#pragma once

#include "Tokenizer.hpp"
#include "InvertedIndex.hpp"
#include "SegmentManager.hpp"
#include "IndexCache.hpp"
#include "StorageEngine.hpp"

namespace surface::index {

class SurfaceIndexEngine {
public:
    explicit SurfaceIndexEngine(
        std::filesystem::path root);

    bool initialize();

    bool index(Document document);

    bool remove(DocumentId id);

    std::vector<DocumentId>
    queryTerm(
        const std::wstring& term) const;

    IndexStatistics statistics() const;

private:
    Tokenizer tokenizer_;
    InvertedIndex index_;
    SegmentManager segments_;

    IndexCache cache_;

    StorageEngine storage_;

    uint64_t nextDocumentId_{1};
};

}
18. Engine implementation
#include "SurfaceIndexEngine.hpp"

namespace surface::index {

SurfaceIndexEngine::SurfaceIndexEngine(
    std::filesystem::path root)
    : storage_(std::move(root)) {
}

bool SurfaceIndexEngine::initialize() {
    return storage_.initialize();
}

bool SurfaceIndexEngine::index(
    Document document) {

    if (document.metadata.id == 0) {
        document.metadata.id =
            nextDocumentId_++;
    }

    auto tokens =
        tokenizer_.tokenize(
            document.content
        );

    /*
     * Reference term mapping.
     *
     * Production implementation should use a persistent
     * dictionary with stable TermIds.
     */

    std::unordered_map<
        std::wstring,
        TermId
    > terms;

    TermId nextTerm = 1;

    for (const auto& token : tokens) {

        auto [it, inserted] =
            terms.emplace(
                token,
                nextTerm
            );

        if (inserted) {
            ++nextTerm;
        }

        segments_.add(
            it->second,
            document.metadata.id
        );
    }

    if (!storage_.write(document)) {
        return false;
    }

    cache_.put(
        std::move(document)
    );

    return true;
}

bool SurfaceIndexEngine::remove(
    DocumentId id) {

    cache_.erase(id);

    return storage_.remove(id);
}

std::vector<DocumentId>
SurfaceIndexEngine::queryTerm(
    const std::wstring& term) const {

    auto tokens =
        tokenizer_.tokenize(term);

    if (tokens.empty()) {
        return {};
    }

    /*
     * Reference implementation only.
     * A persistent term dictionary is required for production.
     */

    return {};
}

IndexStatistics
SurfaceIndexEngine::statistics() const {

    IndexStatistics stats;

    stats.terms =
        index_.termCount();

    stats.postings =
        index_.postingCount();

    stats.documents =
        cache_.get(0).has_value()
        ? 1
        : 0;

    return stats;
}

}

The intentionally incomplete term mapping highlights an important engineering point: a real search engine cannot recreate term IDs independently on every document. The next version needs a persistent dictionary.

19. What the production storage engine should become

The actual high-performance architecture should eventually look more like:

                 Query
                   │
                   ▼
             Query Planner
                   │
          ┌────────┴────────┐
          ▼                 ▼
     Term Dictionary    Metadata Index
          │                 │
          ▼                 ▼
     Posting Lists       Documents
          │                 │
          └────────┬────────┘
                   ▼
                Cache
                   │
                   ▼
             Segment Manager
                   │
       ┌───────────┼───────────┐
       ▼           ▼           ▼
    Segment 1   Segment 2   Segment N
       │           │           │
       └───────────┼───────────┘
                   ▼
             Rust Storage
                   │
             ┌─────┴─────┐
             ▼           ▼
           WAL          NVMe
The really important optimisations
1. Memory mapping

Large immutable index segments can eventually use Windows memory-mapped files:

SSD
 │
 ▼
Memory mapped segment
 │
 ▼
CPU cache
 │
 ▼
Query

That avoids repeatedly copying large index structures through user-space buffers.

2. Compressed postings

Instead of:

10
42
91
133

store document-ID deltas:

10
32
49
42

which can then be encoded with variable-byte or SIMD-friendly integer compression.

3. Immutable segments

Once a segment is sealed:

Segment N
   │
   ├── immutable
   ├── searchable
   └── memory-map friendly

New writes go into a new segment.

4. Background compaction

Never stop interactive searching just because indexing is occurring.

Foreground
──────────
Search ───────────────►

Background
──────────────────────
Index → Flush → Compact
5. SSD-aware scheduling

This can integrate directly with the earlier Surface systems:

#1 Battery
     │
     ▼
Index scheduling
     │
     ├── AC → aggressive indexing
     ├── battery → reduced indexing
     └── critical → pause
     
#2 Thermal
     │
     ▼
NVMe/index throttling

#9 Performance
     │
     ▼
CPU/storage budget

#22 Diagnostics
     │
     ▼
Index health telemetry

#23 Analytics
     │
     ▼
I/O performance analysis
20. Tests
TokenizerTests.cpp
#include "Tokenizer.hpp"

#include <cassert>

using namespace surface::index;

int main() {

    Tokenizer tokenizer;

    auto tokens =
        tokenizer.tokenize(
            L"Surface COMPUTE Engine"
        );

    assert(tokens.size() == 3);

    assert(tokens[0] == L"surface");
    assert(tokens[1] == L"compute");
    assert(tokens[2] == L"engine");

    return 0;
}
IndexTests.cpp
#include "InvertedIndex.hpp"

#include <cassert>

using namespace surface::index;

int main() {

    InvertedIndex index;

    index.add(1, 100);
    index.add(1, 200);
    index.add(2, 300);

    auto results =
        index.lookup(1);

    assert(results.size() == 2);

    assert(index.termCount() == 2);
    assert(index.postingCount() == 3);

    return 0;
}
21. CMake
cmake_minimum_required(VERSION 3.25)

project(
    SurfaceLocalIndex
    VERSION 0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
    SurfaceLocalIndex
    STATIC

    cpp/src/Document.cpp
    cpp/src/Tokenizer.cpp
    cpp/src/InvertedIndex.cpp
    cpp/src/QueryEngine.cpp
    cpp/src/Segment.cpp
    cpp/src/SegmentManager.cpp
    cpp/src/IndexCache.cpp
    cpp/src/StorageEngine.cpp
    cpp/src/IndexScheduler.cpp
    cpp/src/SurfaceIndexEngine.cpp
)

target_include_directories(
    SurfaceLocalIndex
    PUBLIC
        cpp/include
)

if(WIN32)

    target_link_libraries(
        SurfaceLocalIndex
        PRIVATE
            advapi32
    )

endif()

add_executable(
    SurfaceLocalIndexDemo
    cpp/src/main.cpp
)

target_link_libraries(
    SurfaceLocalIndexDemo
    PRIVATE
        SurfaceLocalIndex
)

enable_testing()

add_executable(
    SurfaceIndexTests
    tests/TokenizerTests.cpp
)

target_link_libraries(
    SurfaceIndexTests
    PRIVATE
        SurfaceLocalIndex
)

add_test(
    NAME SurfaceIndexTests
    COMMAND SurfaceIndexTests
)





Project
SurfaceCryptoProtocols/
├── Cargo.toml
├── README.md
├── src/
│   ├── lib.rs
│   ├── error.rs
│   ├── types.rs
│   ├── random.rs
│   ├── hashing.rs
│   ├── aead.rs
│   ├── signing.rs
│   ├── key_exchange.rs
│   ├── key_derivation.rs
│   ├── certificate.rs
│   ├── identity.rs
│   ├── session.rs
│   ├── protocol.rs
│   ├── replay.rs
│   ├── ratelimit.rs
│   ├── audit.rs
│   └── ffi.rs
└── tests/
    ├── crypto_tests.rs
    ├── protocol_tests.rs
    └── replay_tests.rs
1. Dependencies

Use well-established implementations rather than handwritten AES, ChaCha20, SHA-2, elliptic curves, etc.

Cargo.toml
[package]
name = "surface_crypto_protocols"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["rlib", "cdylib"]

[dependencies]
aes-gcm = "0.10"
chacha20poly1305 = "0.10"
sha2 = "0.10"
hkdf = "0.12"
rand = "0.8"
ed25519-dalek = "2"
x25519-dalek = "2"
zeroize = { version = "1", features = ["derive"] }
thiserror = "2"
serde = { version = "1", features = ["derive"] }

For a real product, pin exact versions and maintain a reviewed dependency/SBOM policy.

2. Core types
src/types.rs
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ProtocolVersion {
    V1 = 1,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CipherSuite {
    Aes256Gcm,
    ChaCha20Poly1305,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    New,
    Established,
    Closing,
    Closed,
    Failed,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProtocolHeader {
    pub version: u8,
    pub message_type: u16,
    pub sequence: u64,
    pub payload_length: u32,
}

#[derive(Debug, Clone)]
pub struct SessionMetadata {
    pub session_id: [u8; 16],
    pub state: SessionState,
    pub cipher: CipherSuite,
}
3. Error handling
src/error.rs
use thiserror::Error;

#[derive(Debug, Error)]
pub enum CryptoError {
    #[error("invalid key")]
    InvalidKey,

    #[error("invalid nonce")]
    InvalidNonce,

    #[error("authentication failed")]
    AuthenticationFailed,

    #[error("invalid protocol message")]
    InvalidMessage,

    #[error("replay detected")]
    ReplayDetected,

    #[error("sequence number exhausted")]
    SequenceExhausted,

    #[error("session is not established")]
    SessionNotEstablished,

    #[error("cryptographic operation failed")]
    CryptoFailure,
}

pub type Result<T> = std::result::Result<T, CryptoError>;

The protocol layer should deliberately avoid exposing detailed authentication failures to remote peers.

4. Secure random generation
src/random.rs
use rand::rngs::OsRng;
use rand::RngCore;

use crate::error::Result;

pub fn random_bytes<const N: usize>() -> Result<[u8; N]> {
    let mut output = [0u8; N];

    OsRng.fill_bytes(&mut output);

    Ok(output)
}

This uses the operating system's cryptographically secure random source.

5. Hashing
src/hashing.rs
use sha2::{Digest, Sha256};

pub fn sha256(data: &[u8]) -> [u8; 32] {
    let digest = Sha256::digest(data);

    digest.into()
}

Keep hashing as a tiny abstraction so the protocol code isn't coupled directly to a particular implementation.

6. HKDF key derivation

This is preferable to inventing a custom key derivation construction.

src/key_derivation.rs
use hkdf::Hkdf;
use sha2::Sha256;

use crate::error::{CryptoError, Result};

pub fn derive_key(
    secret: &[u8],
    salt: &[u8],
    info: &[u8],
) -> Result<[u8; 32]> {

    let hk =
        Hkdf::<Sha256>::new(
            Some(salt),
            secret,
        );

    let mut output = [0u8; 32];

    hk.expand(
        info,
        &mut output,
    )
    .map_err(|_| CryptoError::CryptoFailure)?;

    Ok(output)
}
7. Authenticated encryption

For general-purpose protocol encryption, AES-256-GCM and ChaCha20-Poly1305 are good standard constructions.

src/aead.rs
use aes_gcm::{
    Aes256Gcm,
    KeyInit,
    Nonce,
    aead::{
        Aead,
        Payload,
    },
};

use crate::error::{
    CryptoError,
    Result,
};

pub struct AesGcmCipher {
    cipher: Aes256Gcm,
}

impl AesGcmCipher {

    pub fn new(
        key: &[u8; 32]
    ) -> Self {

        let cipher =
            Aes256Gcm::new_from_slice(
                key
            )
            .expect("32-byte AES key");

        Self { cipher }
    }

    pub fn encrypt(
        &self,
        nonce: &[u8; 12],
        plaintext: &[u8],
        associated_data: &[u8],
    ) -> Result<Vec<u8>> {

        self.cipher
            .encrypt(
                Nonce::from_slice(nonce),
                Payload {
                    msg: plaintext,
                    aad: associated_data,
                },
            )
            .map_err(
                |_| CryptoError::CryptoFailure
            )
    }

    pub fn decrypt(
        &self,
        nonce: &[u8; 12],
        ciphertext: &[u8],
        associated_data: &[u8],
    ) -> Result<Vec<u8>> {

        self.cipher
            .decrypt(
                Nonce::from_slice(nonce),
                Payload {
                    msg: ciphertext,
                    aad: associated_data,
                },
            )
            .map_err(
                |_| CryptoError::AuthenticationFailed
            )
    }
}

Nonce reuse is catastrophic for GCM, so the session layer—not arbitrary callers—should own nonce construction.

8. ChaCha20-Poly1305
use chacha20poly1305::{
    ChaCha20Poly1305,
    KeyInit,
    Nonce,
    aead::{
        Aead,
        Payload,
    },
};

use crate::error::{
    CryptoError,
    Result,
};

pub struct ChaChaCipher {
    cipher: ChaCha20Poly1305,
}

impl ChaChaCipher {

    pub fn new(
        key: &[u8; 32]
    ) -> Self {

        Self {
            cipher:
                ChaCha20Poly1305::new_from_slice(
                    key
                )
                .expect("32-byte key"),
        }
    }

    pub fn encrypt(
        &self,
        nonce: &[u8; 12],
        plaintext: &[u8],
        aad: &[u8],
    ) -> Result<Vec<u8>> {

        self.cipher
            .encrypt(
                Nonce::from_slice(nonce),
                Payload {
                    msg: plaintext,
                    aad,
                },
            )
            .map_err(
                |_| CryptoError::CryptoFailure
            )
    }

    pub fn decrypt(
        &self,
        nonce: &[u8; 12],
        ciphertext: &[u8],
        aad: &[u8],
    ) -> Result<Vec<u8>> {

        self.cipher
            .decrypt(
                Nonce::from_slice(nonce),
                Payload {
                    msg: ciphertext,
                    aad,
                },
            )
            .map_err(
                |_| CryptoError::AuthenticationFailed
            )
    }
}
9. Signing

Ed25519 provides a useful modern signing primitive for application-level signed messages.

src/signing.rs
use ed25519_dalek::{
    Signature,
    Signer,
    SigningKey,
    Verifier,
    VerifyingKey,
};

use crate::error::{
    CryptoError,
    Result,
};

pub struct IdentityKey {
    signing: SigningKey,
}

impl IdentityKey {

    pub fn from_bytes(
        bytes: &[u8; 32]
    ) -> Self {

        Self {
            signing:
                SigningKey::from_bytes(bytes),
        }
    }

    pub fn public_key(
        &self
    ) -> VerifyingKey {

        self.signing.verifying_key()
    }

    pub fn sign(
        &self,
        message: &[u8]
    ) -> Signature {

        self.signing.sign(message)
    }
}

pub fn verify(
    public_key: &VerifyingKey,
    message: &[u8],
    signature: &Signature,
) -> Result<()> {

    public_key
        .verify(message, signature)
        .map_err(
            |_| CryptoError::AuthenticationFailed
        )
}

Private keys should not be casually exportable from the Surface security layer.

10. Key exchange

Use X25519 for ephemeral Diffie-Hellman style key agreement.

src/key_exchange.rs
use rand::rngs::OsRng;
use x25519_dalek::{
    EphemeralSecret,
    PublicKey,
};

pub struct EphemeralKeyExchange {
    secret: EphemeralSecret,
    public: PublicKey,
}

impl EphemeralKeyExchange {

    pub fn generate() -> Self {

        let secret =
            EphemeralSecret::random_from_rng(
                OsRng
            );

        let public =
            PublicKey::from(&secret);

        Self {
            secret,
            public,
        }
    }

    pub fn public_key(
        &self
    ) -> PublicKey {
        self.public
    }

    pub fn derive(
        self,
        peer: &PublicKey,
    ) -> [u8; 32] {

        self.secret
            .diffie_hellman(peer)
            .to_bytes()
    }
}

This gives us:

Surface A                         Surface B

ephemeral key A                   ephemeral key B
      │                                  │
      └────────── X25519 ────────────────┘
                     │
                     ▼
               shared secret
                     │
                     ▼
                    HKDF
                     │
                     ▼
              session encryption
11. Replay protection

This is essential for device-control protocols.

A valid old command must not be reusable by an attacker.

src/replay.rs
use std::collections::VecDeque;

use crate::error::{
    CryptoError,
    Result,
};

pub struct ReplayWindow {
    highest: u64,
    window: u64,
    seen: VecDeque<u64>,
}

impl ReplayWindow {

    pub fn new(window: u64) -> Self {

        Self {
            highest: 0,
            window,
            seen: VecDeque::new(),
        }
    }

    pub fn accept(
        &mut self,
        sequence: u64,
    ) -> Result<()> {

        if sequence == 0 {
            return Err(
                CryptoError::ReplayDetected
            );
        }

        if sequence + self.window <
           self.highest {

            return Err(
                CryptoError::ReplayDetected
            );
        }

        if self.seen.contains(&sequence) {
            return Err(
                CryptoError::ReplayDetected
            );
        }

        if sequence > self.highest {
            self.highest = sequence;
        }

        self.seen.push_back(sequence);

        while self.seen.len() >
              self.window as usize {

            self.seen.pop_front();
        }

        Ok(())
    }
}

For production, this should become a bitset sliding window, avoiding linear contains() searches.

12. Session state
src/session.rs
use zeroize::Zeroize;

use crate::{
    error::{
        CryptoError,
        Result,
    },
    types::{
        CipherSuite,
        SessionState,
    },
    replay::ReplayWindow,
};

#[derive(Zeroize)]
#[zeroize(drop)]
pub struct SessionKey {
    key: [u8; 32],
}

impl SessionKey {

    pub fn new(key: [u8; 32]) -> Self {
        Self { key }
    }

    pub fn bytes(&self) -> &[u8; 32] {
        &self.key
    }
}

pub struct Session {
    state: SessionState,
    cipher: CipherSuite,
    key: SessionKey,
    send_sequence: u64,
    replay: ReplayWindow,
}

impl Session {

    pub fn establish(
        key: [u8; 32],
        cipher: CipherSuite,
    ) -> Self {

        Self {
            state: SessionState::Established,
            cipher,
            key: SessionKey::new(key),
            send_sequence: 0,
            replay: ReplayWindow::new(128),
        }
    }

    pub fn state(&self) -> SessionState {
        self.state
    }

    pub fn cipher(&self) -> CipherSuite {
        self.cipher
    }

    pub fn next_sequence(
        &mut self
    ) -> Result<u64> {

        if self.send_sequence ==
           u64::MAX {

            return Err(
                CryptoError::SequenceExhausted
            );
        }

        self.send_sequence += 1;

        Ok(self.send_sequence)
    }

    pub fn accept_sequence(
        &mut self,
        sequence: u64,
    ) -> Result<()> {

        self.replay.accept(sequence)
    }

    pub fn key(&self) -> &[u8; 32] {
        self.key.bytes()
    }
}
13. Protocol envelope

Every protected message should have a deterministic envelope.

┌───────────────────────────────────────┐
│ Protocol version                      │
├───────────────────────────────────────┤
│ Message type                          │
├───────────────────────────────────────┤
│ Session ID                            │
├───────────────────────────────────────┤
│ Sequence number                       │
├───────────────────────────────────────┤
│ Nonce                                 │
├───────────────────────────────────────┤
│ Ciphertext                            │
├───────────────────────────────────────┤
│ Authentication tag                   │
└───────────────────────────────────────┘
src/protocol.rs
use crate::{
    error::{CryptoError, Result},
    session::Session,
};

pub struct SecureMessage {
    pub message_type: u16,
    pub sequence: u64,
    pub nonce: [u8; 12],
    pub ciphertext: Vec<u8>,
}

impl SecureMessage {

    pub fn validate(
        &self,
        session: &mut Session,
    ) -> Result<()> {

        session.accept_sequence(
            self.sequence
        )?;

        if self.ciphertext.is_empty() {
            return Err(
                CryptoError::InvalidMessage
            );
        }

        Ok(())
    }
}
14. Identity layer

This connects directly with the earlier #10 Security / trusted hardware and #11 Rust security services.

use ed25519_dalek::VerifyingKey;

pub struct DeviceIdentity {
    pub device_id: [u8; 16],
    pub signing_key: Option<VerifyingKey>,
    pub trusted_hardware: bool,
}

impl DeviceIdentity {

    pub fn is_trusted(&self) -> bool {
        self.trusted_hardware &&
            self.signing_key.is_some()
    }
}

The production private key should instead be backed by Windows CNG/NCrypt, TPM-backed keys or another hardware-backed key provider where appropriate.

The Rust protocol layer should request signatures without receiving an exportable private key.

15. Audit events

Cryptographic security also needs observability.

audit.rs
use std::time::{
    SystemTime,
    UNIX_EPOCH,
};

#[derive(Debug, Clone, Copy)]
pub enum AuditEvent {
    SessionCreated,
    AuthenticationSucceeded,
    AuthenticationFailed,
    ReplayRejected,
    MessageRejected,
    SessionClosed,
}

#[derive(Debug)]
pub struct AuditRecord {
    pub timestamp: u64,
    pub event: AuditEvent,
}

pub fn record(
    event: AuditEvent
) -> AuditRecord {

    let timestamp =
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();

    AuditRecord {
        timestamp,
        event,
    }
}

In the Surface architecture this can feed the existing Rust Device Services #21 and Python Diagnostics #22.

16. Rate limiting

Cryptographic operations themselves can become a DoS vector.

pub struct CryptoRateLimiter {
    operations: u32,
    limit: u32,
}

impl CryptoRateLimiter {

    pub fn new(limit: u32) -> Self {
        Self {
            operations: 0,
            limit,
        }
    }

    pub fn allow(&mut self) -> bool {

        if self.operations >= self.limit {
            return false;
        }

        self.operations += 1;

        true
    }

    pub fn reset(&mut self) {
        self.operations = 0;
    }
}

Production would use a time-based token bucket.

17. FFI boundary to C++

The C++ Surface hardware stack can consume the Rust security decision.

ffi.rs
use crate::{
    policy::SecurityDecision,
};

#[repr(C)]
pub struct CSecurityDecision {
    pub allowed: u8,
    pub security_level: u32,
    pub reason: u32,
}

#[no_mangle]
pub extern "C" fn
surface_crypto_validate_message(
    sequence: u64,
    highest_sequence: u64,
) -> CSecurityDecision {

    if sequence == 0 ||
       sequence <= highest_sequence {

        return CSecurityDecision {
            allowed: 0,
            security_level: 0,
            reason: 1,
        };
    }

    CSecurityDecision {
        allowed: 1,
        security_level: 2,
        reason: 0,
    }
}

The real FFI should additionally include:

ABI version
struct size
capability flags
reserved fields
error code

so future Rust releases don't silently break the C++ layer.

18. lib.rs
pub mod error;
pub mod types;
pub mod random;
pub mod hashing;
pub mod aead;
pub mod signing;
pub mod key_exchange;
pub mod key_derivation;
pub mod certificate;
pub mod identity;
pub mod session;
pub mod protocol;
pub mod replay;
pub mod ratelimit;
pub mod audit;
pub mod ffi;
19. Critical tests
tests/crypto_tests.rs
use surface_crypto_protocols::{
    aead::AesGcmCipher,
    hashing::sha256,
};

#[test]
fn sha256_is_deterministic() {

    let a = sha256(b"surface");
    let b = sha256(b"surface");

    assert_eq!(a, b);
}

#[test]
fn aes_gcm_round_trip() {

    let key = [7u8; 32];
    let nonce = [9u8; 12];

    let cipher =
        AesGcmCipher::new(&key);

    let plaintext =
        b"Surface secure message";

    let encrypted =
        cipher
            .encrypt(
                &nonce,
                plaintext,
                b"header",
            )
            .unwrap();

    let decrypted =
        cipher
            .decrypt(
                &nonce,
                &encrypted,
                b"header",
            )
            .unwrap();

    assert_eq!(
        decrypted,
        plaintext
    );
}

#[test]
fn modified_ciphertext_fails() {

    let key = [1u8; 32];
    let nonce = [2u8; 12];

    let cipher =
        AesGcmCipher::new(&key);

    let mut encrypted =
        cipher
            .encrypt(
                &nonce,
                b"hello",
                b"",
            )
            .unwrap();

    encrypted[0] ^= 1;

    assert!(
        cipher
            .decrypt(
                &nonce,
                &encrypted,
                b"",
            )
            .is_err()
    );
}
20. Replay tests
use surface_crypto_protocols::replay::ReplayWindow;

#[test]
fn replay_is_rejected() {

    let mut window =
        ReplayWindow::new(128);

    assert!(
        window.accept(1).is_ok()
    );

    assert!(
        window.accept(1).is_err()
    );
}

#[test]
fn zero_sequence_is_rejected() {

    let mut window =
        ReplayWindow::new(128);

    assert!(
        window.accept(0).is_err()
    );
}





Project structure
SurfaceAutomatedTesting/
├── pyproject.toml
├── README.md
├── config/
│   ├── lab.toml
│   ├── devices.toml
│   └── limits.toml
│
├── surface_test/
│   ├── __init__.py
│   ├── cli.py
│   ├── config.py
│   │
│   ├── core/
│   │   ├── test_case.py
│   │   ├── test_suite.py
│   │   ├── context.py
│   │   ├── result.py
│   │   ├── runner.py
│   │   └── scheduler.py
│   │
│   ├── dut/
│   │   ├── device.py
│   │   ├── connection.py
│   │   ├── telemetry.py
│   │   ├── power.py
│   │   └── reboot.py
│   │
│   ├── instruments/
│   │   ├── instrument.py
│   │   ├── power_supply.py
│   │   ├── digital_multimeter.py
│   │   ├── oscilloscope.py
│   │   ├── temperature.py
│   │   └── usb.py
│   │
│   ├── tests/
│   │   ├── battery.py
│   │   ├── thermal.py
│   │   ├── usb.py
│   │   ├── display.py
│   │   ├── radio.py
│   │   ├── performance.py
│   │   └── boot.py
│   │
│   ├── analysis/
│   │   ├── statistics.py
│   │   ├── limits.py
│   │   ├── anomalies.py
│   │   ├── regression.py
│   │   └── correlation.py
│   │
│   ├── reporting/
│   │   ├── json_report.py
│   │   ├── html_report.py
│   │   └── junit.py
│   │
│   └── storage/
│       ├── database.py
│       └── artifacts.py
│
├── testplans/
│   ├── smoke.yaml
│   ├── battery.yaml
│   ├── thermal.yaml
│   ├── usb.yaml
│   └── full_validation.yaml
│
└── tests/
    ├── test_runner.py
    ├── test_limits.py
    ├── test_statistics.py
    └── test_simulated_dut.py
1. Dependencies
pyproject.toml
[project]
name = "surface-automated-testing"
version = "0.1.0"
requires-python = ">=3.11"

dependencies = [
    "pydantic>=2",
    "numpy>=2",
    "pandas>=2",
    "pyyaml>=6",
    "scipy>=1.12",
    "jinja2>=3.1"
]

[project.optional-dependencies]
lab = [
    "pyvisa>=1.14",
    "pyserial>=3.5",
    "psutil>=6"
]

test = [
    "pytest>=8",
    "pytest-asyncio>=0.24"
]

[project.scripts]
surface-test = "surface_test.cli:main"

The hardware dependencies are optional so the complete framework can run in a simulated lab on a developer workstation.

2. Test result model
core/result.py
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import Enum
from typing import Any


class TestStatus(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    ERROR = "ERROR"
    SKIPPED = "SKIPPED"


@dataclass
class Measurement:
    name: str
    value: float
    unit: str
    timestamp: datetime = field(
        default_factory=lambda:
        datetime.now(timezone.utc)
    )


@dataclass
class TestResult:
    test_id: str
    name: str
    status: TestStatus
    duration_seconds: float

    measurements: list[Measurement] = field(
        default_factory=list
    )

    messages: list[str] = field(
        default_factory=list
    )

    metadata: dict[str, Any] = field(
        default_factory=dict
    )

    def passed(self) -> bool:
        return self.status == TestStatus.PASS
3. Test context

Everything a test needs is injected through the context.

core/context.py
from dataclasses import dataclass

from surface_test.dut.device import DUT
from surface_test.instruments.power_supply import PowerSupply
from surface_test.instruments.temperature import TemperatureSource


@dataclass
class TestContext:
    dut: DUT
    power_supply: PowerSupply | None = None
    temperature: TemperatureSource | None = None

This is important because a test should not contain hard-coded VISA addresses, serial ports or machine-specific paths.

4. Test case abstraction
core/test_case.py
from abc import ABC, abstractmethod

from surface_test.core.context import TestContext
from surface_test.core.result import TestResult


class TestCase(ABC):

    test_id: str
    name: str

    @abstractmethod
    def run(
        self,
        context: TestContext
    ) -> TestResult:
        ...
5. DUT abstraction
dut/device.py
from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass
class DUTInfo:
    serial: str
    model: str
    firmware: str
    os_version: str


class DUT:

    def __init__(
        self,
        info: DUTInfo,
    ) -> None:

        self.info = info
        self.connected = False

    def connect(self) -> None:
        self.connected = True

    def disconnect(self) -> None:
        self.connected = False

    def command(
        self,
        name: str,
        **arguments: Any,
    ) -> Any:

        if not self.connected:
            raise RuntimeError(
                "DUT is not connected"
            )

        # Production implementation:
        # authenticated Surface test service / WinRM /
        # PowerShell remoting / dedicated test agent.
        return {
            "command": name,
            "arguments": arguments,
        }

    def telemetry(self) -> dict[str, float]:

        if not self.connected:
            raise RuntimeError(
                "DUT is not connected"
            )

        return {
            "cpu_percent": 0.0,
            "gpu_percent": 0.0,
            "temperature_c": 0.0,
            "battery_percent": 0.0,
        }

The production implementation should use an authenticated test agent rather than arbitrary remote shell access.

6. Power supply
instruments/power_supply.py
from abc import ABC, abstractmethod


class PowerSupply(ABC):

    @abstractmethod
    def set_voltage(
        self,
        volts: float,
    ) -> None:
        ...

    @abstractmethod
    def set_current_limit(
        self,
        amps: float,
    ) -> None:
        ...

    @abstractmethod
    def output(
        self,
        enabled: bool,
    ) -> None:
        ...

    @abstractmethod
    def measure_voltage(self) -> float:
        ...

    @abstractmethod
    def measure_current(self) -> float:
        ...


class SimulatedPowerSupply(
    PowerSupply
):

    def __init__(self) -> None:

        self.voltage = 20.0
        self.current_limit = 5.0
        self.enabled = False

    def set_voltage(
        self,
        volts: float,
    ) -> None:

        self.voltage = volts

    def set_current_limit(
        self,
        amps: float,
    ) -> None:

        self.current_limit = amps

    def output(
        self,
        enabled: bool,
    ) -> None:

        self.enabled = enabled

    def measure_voltage(self) -> float:
        return self.voltage

    def measure_current(self) -> float:
        return (
            self.current_limit
            if self.enabled
            else 0.0
        )
7. Temperature instrumentation
from abc import ABC, abstractmethod


class TemperatureSource(ABC):

    @abstractmethod
    def set_temperature(
        self,
        temperature_c: float,
    ) -> None:
        ...

    @abstractmethod
    def read_temperature(self) -> float:
        ...


class SimulatedTemperatureSource(
    TemperatureSource
):

    def __init__(self) -> None:
        self.temperature_c = 25.0

    def set_temperature(
        self,
        temperature_c: float,
    ) -> None:

        self.temperature_c = temperature_c

    def read_temperature(self) -> float:
        return self.temperature_c
8. Battery validation

This test connects directly to #1 Power, #16 Julia Battery, and #23 Performance Analytics.

tests/battery.py
import time

from surface_test.core.test_case import TestCase
from surface_test.core.context import TestContext
from surface_test.core.result import (
    Measurement,
    TestResult,
    TestStatus,
)


class BatteryIdleTest(TestCase):

    test_id = "BAT-001"
    name = "Battery idle discharge"

    def run(
        self,
        context: TestContext,
    ) -> TestResult:

        start = time.monotonic()

        try:

            telemetry = (
                context.dut.telemetry()
            )

            battery = telemetry[
                "battery_percent"
            ]

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=TestStatus.PASS,
                duration_seconds=(
                    time.monotonic() - start
                ),
                measurements=[
                    Measurement(
                        name="battery_percent",
                        value=battery,
                        unit="%",
                    )
                ],
            )

        except Exception as exc:

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=TestStatus.ERROR,
                duration_seconds=(
                    time.monotonic() - start
                ),
                messages=[str(exc)],
            )

A production battery test would run for hours and record:

SOC
voltage
current
power
temperature
charge rate
discharge rate
battery health
CPU load
display refresh
Wi-Fi state
thermal state
9. Thermal test
tests/thermal.py
import time

from surface_test.core.test_case import TestCase
from surface_test.core.context import TestContext
from surface_test.core.result import (
    Measurement,
    TestResult,
    TestStatus,
)


class ThermalStressTest(TestCase):

    test_id = "THM-001"
    name = "Thermal stress protection"

    def run(
        self,
        context: TestContext,
    ) -> TestResult:

        start = time.monotonic()

        try:

            context.dut.command(
                "start_cpu_stress",
                duration_seconds=60,
            )

            telemetry = (
                context.dut.telemetry()
            )

            temperature = telemetry[
                "temperature_c"
            ]

            status = (
                TestStatus.PASS
                if temperature < 100.0
                else TestStatus.FAIL
            )

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=status,
                duration_seconds=(
                    time.monotonic() - start
                ),
                measurements=[
                    Measurement(
                        name="temperature",
                        value=temperature,
                        unit="°C",
                    )
                ],
            )

        except Exception as exc:

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=TestStatus.ERROR,
                duration_seconds=(
                    time.monotonic() - start
                ),
                messages=[str(exc)],
            )
10. USB test

This connects directly to #27 USB/Thunderbolt.

import time

from surface_test.core.test_case import TestCase
from surface_test.core.context import TestContext
from surface_test.core.result import (
    Measurement,
    TestResult,
    TestStatus,
)


class USBEnumerationTest(TestCase):

    test_id = "USB-001"
    name = "USB device enumeration"

    def run(
        self,
        context: TestContext,
    ) -> TestResult:

        start = time.monotonic()

        try:

            result = context.dut.command(
                "enumerate_usb"
            )

            device_count = len(
                result.get("devices", [])
            )

            status = (
                TestStatus.PASS
                if device_count > 0
                else TestStatus.FAIL
            )

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=status,
                duration_seconds=(
                    time.monotonic() - start
                ),
                measurements=[
                    Measurement(
                        name="usb_devices",
                        value=device_count,
                        unit="devices",
                    )
                ],
            )

        except Exception as exc:

            return TestResult(
                test_id=self.test_id,
                name=self.name,
                status=TestStatus.ERROR,
                duration_seconds=(
                    time.monotonic() - start
                ),
                messages=[str(exc)],
            )
11. Test runner
core/runner.py
from dataclasses import dataclass

from surface_test.core.context import TestContext
from surface_test.core.result import (
    TestResult,
    TestStatus,
)
from surface_test.core.test_case import TestCase


@dataclass
class TestRun:

    results: list[TestResult]

    @property
    def passed(self) -> int:

        return sum(
            r.status == TestStatus.PASS
            for r in self.results
        )

    @property
    def failed(self) -> int:

        return sum(
            r.status == TestStatus.FAIL
            for r in self.results
        )


class TestRunner:

    def __init__(
        self,
        context: TestContext,
    ) -> None:

        self.context = context

    def run(
        self,
        tests: list[TestCase],
    ) -> TestRun:

        results = []

        for test in tests:

            result = test.run(
                self.context
            )

            results.append(result)

        return TestRun(results)
12. Test scheduler

Large hardware validation labs need controlled sequencing.

from dataclasses import dataclass
from enum import IntEnum


class Priority(IntEnum):
    SAFETY = 0
    CRITICAL = 10
    NORMAL = 20
    BACKGROUND = 30


@dataclass
class ScheduledTest:
    test: object
    priority: Priority


class TestScheduler:

    def __init__(self) -> None:
        self.tests: list[
            ScheduledTest
        ] = []

    def add(
        self,
        test: object,
        priority: Priority = Priority.NORMAL,
    ) -> None:

        self.tests.append(
            ScheduledTest(
                test=test,
                priority=priority,
            )
        )

    def ordered(self) -> list[ScheduledTest]:

        return sorted(
            self.tests,
            key=lambda item: item.priority,
        )

Safety tests always run before destructive/high-load tests.

13. Measurement limits
analysis/limits.py
from dataclasses import dataclass


@dataclass(frozen=True)
class Limit:

    minimum: float | None = None
    maximum: float | None = None

    def check(
        self,
        value: float,
    ) -> bool:

        if (
            self.minimum is not None
            and value < self.minimum
        ):
            return False

        if (
            self.maximum is not None
            and value > self.maximum
        ):
            return False

        return True

Example:

THERMAL_LIMIT = Limit(
    maximum=100.0
)

USB_LATENCY_LIMIT = Limit(
    maximum=10.0
)

BOOT_TIME_LIMIT = Limit(
    maximum=30.0
)
14. Statistical analysis
analysis/statistics.py
from dataclasses import dataclass

import numpy as np


@dataclass
class Statistics:

    count: int
    mean: float
    minimum: float
    maximum: float
    standard_deviation: float


def describe(
    values: list[float],
) -> Statistics:

    if not values:
        raise ValueError(
            "No measurements"
        )

    array = np.asarray(
        values,
        dtype=float,
    )

    return Statistics(
        count=len(array),
        mean=float(np.mean(array)),
        minimum=float(np.min(array)),
        maximum=float(np.max(array)),
        standard_deviation=float(
            np.std(array)
        ),
    )

This becomes extremely useful for comparing:

Surface unit A
Surface unit B
Surface unit C
Surface unit D

rather than simply saying "pass/fail."

15. Anomaly detection
analysis/anomalies.py
import numpy as np


def zscore_anomalies(
    values: list[float],
    threshold: float = 3.0,
) -> list[int]:

    if len(values) < 3:
        return []

    data = np.asarray(
        values,
        dtype=float,
    )

    mean = np.mean(data)
    std = np.std(data)

    if std == 0:
        return []

    scores = np.abs(
        (data - mean) / std
    )

    return [
        int(index)
        for index, score
        in enumerate(scores)
        if score > threshold
    ]

This is where Python can detect subtle manufacturing/test-lab problems.

16. Regression detection

A critical capability:

                 Performance
                     │
                     │      ●
                     │   ●
                     │ ●
                     │●
                     └────────────────
                       Build / Firmware
analysis/regression.py
from dataclasses import dataclass


@dataclass
class Regression:

    baseline: float
    current: float
    percentage: float


def compare(
    baseline: float,
    current: float,
) -> Regression:

    if baseline == 0:
        raise ValueError(
            "Baseline cannot be zero"
        )

    percentage = (
        (current - baseline)
        / baseline
        * 100.0
    )

    return Regression(
        baseline=baseline,
        current=current,
        percentage=percentage,
    )

Now firmware builds can automatically produce:

Build 2026.09.21
────────────────────────

Boot time          -3.2%
Battery runtime    +2.7%
CPU performance    +0.8%
GPU performance    -1.4%
Thermal peak       +1.1°C
USB throughput     -0.4%
Wi-Fi latency      +2.1ms
17. JSON reporting
reporting/json_report.py
import json
from dataclasses import asdict

from surface_test.core.runner import TestRun


def write_report(
    run: TestRun,
    path: str,
) -> None:

    payload = {
        "summary": {
            "total": len(run.results),
            "passed": run.passed,
            "failed": run.failed,
        },
        "results": [
            asdict(result)
            for result in run.results
        ],
    }

    with open(
        path,
        "w",
        encoding="utf-8",
    ) as file:

        json.dump(
            payload,
            file,
            indent=2,
            default=str,
        )
18. JUnit output

This allows the Surface hardware lab to plug into CI systems.

import xml.etree.ElementTree as ET

from surface_test.core.runner import TestRun
from surface_test.core.result import TestStatus


def write_junit(
    run: TestRun,
    path: str,
) -> None:

    suite = ET.Element(
        "testsuite",
        {
            "tests": str(
                len(run.results)
            ),
            "failures": str(run.failed),
        },
    )

    for result in run.results:

        case = ET.SubElement(
            suite,
            "testcase",
            {
                "name": result.name,
                "time": str(
                    result.duration_seconds
                ),
            },
        )

        if result.status == TestStatus.FAIL:

            failure = ET.SubElement(
                case,
                "failure",
            )

            failure.text = "\n".join(
                result.messages
            )

        elif result.status == TestStatus.ERROR:

            error = ET.SubElement(
                case,
                "error",
            )

            error.text = "\n".join(
                result.messages
            )

    ET.ElementTree(suite).write(
        path,
        encoding="utf-8",
        xml_declaration=True,
    )
19. Test plan
testplans/smoke.yaml
name: Surface Smoke Test

tests:
  - id: BOOT-001
    name: Boot validation

  - id: USB-001
    name: USB device enumeration

  - id: BAT-001
    name: Battery idle discharge

  - id: THM-001
    name: Thermal protection

A much larger production plan could be:

name: Surface Full Validation

stages:

  boot:
    - BOOT-001
    - BOOT-002
    - BOOT-003

  power:
    - BAT-001
    - BAT-002
    - BAT-003
    - PWR-001
    - PWR-002

  thermal:
    - THM-001
    - THM-002
    - THM-003

  input:
    - PEN-001
    - TOUCH-001

  display:
    - DISP-001
    - DISP-002
    - DISP-003

  connectivity:
    - WIFI-001
    - BT-001
    - USB-001
    - TB-001

  performance:
    - CPU-001
    - GPU-001
    - NPU-001

  security:
    - SEC-001
    - SEC-002

  endurance:
    - END-001
    - END-002
20. Hardware test matrix

This gives #30 a much bigger role than a normal pytest suite.

Subsystem	Automated test
Battery	charge/discharge/runtime
PMIC	voltage/current/fault
Thermal	heat soak/fan response
CPU	sustained workload/throttling
GPU	rendering/thermal/performance
NPU	inference throughput/power
Display	refresh/HDR/brightness
Touch	latency/multi-touch
Pen	pressure/latency/accuracy
Camera	exposure/focus/thermal
Audio	frequency/SNR/latency
Wi-Fi	throughput/roaming/latency
Bluetooth	pairing/throughput/reconnect
USB	enumeration/throughput/hotplug
Thunderbolt	topology/security/throughput
Storage	IOPS/latency/thermal
Security	boot/key/signature/integrity
Firmware	update/recovery/rollback
Sleep	suspend/resume/Modern Standby
Docking	attach/detach/display/network
Endurance	multi-hour/multi-day testing
21. The really useful part: automated correlation

This is where #30 connects with #23 Julia/Python Performance Analytics.

Imagine a 12-hour test run produces:

timestamp
CPU load
GPU load
NPU load
CPU frequency
GPU frequency
battery current
battery voltage
battery temperature
CPU temperature
GPU temperature
fan RPM
display refresh
Wi-Fi RSSI
USB throughput
NVMe latency

Python can then correlate them:

from surface_test.analysis.statistics import describe

temperatures = [
    62.1,
    64.8,
    67.2,
    71.5,
    73.0,
]

stats = describe(temperatures)

print(stats)

Then feed the resulting dataset into the existing Julia analytics system:

                Hardware Lab
                     │
                     ▼
             Python Test System
                     │
                     ▼
              Raw telemetry
                     │
          ┌──────────┴──────────┐
          ▼                     ▼
       Python                Julia #23
     diagnostics           optimisation
          │                     │
          └──────────┬──────────┘
                     ▼
              Engineering Report
22. Automated regression gate

This is the feature I'd make central to the system.

def regression_allowed(
    baseline: float,
    current: float,
    maximum_regression_percent: float,
) -> bool:

    if baseline == 0:
        return False

    regression = (
        (baseline - current)
        / baseline
        * 100.0
    )

    return regression <= (
        maximum_regression_percent
    )

So a firmware build can automatically be tested:

             New Surface firmware
                      │
                      ▼
                Flash DUT
                      │
                      ▼
              Run 150 tests
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
      Functional              Performance
          │                       │
          └───────────┬───────────┘
                      ▼
                Regression
                   engine
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
       Accept                  Investigate
23. Safety architecture

Because this is controlling real hardware, Python must never be allowed to blindly execute arbitrary commands.

Python Lab Controller
        │
        │ authenticated protocol
        ▼
Surface Test Agent
        │
        ├── capability checks
        ├── command validation
        ├── emergency limits
        ├── watchdog
        └── audit logging
        │
        ▼
Surface DUT

For example:

MAX_SAFE_VOLTAGE = 21.0
MAX_SAFE_TEMPERATURE = 100.0
MAX_TEST_CURRENT = 10.0


def validate_power_request(
    voltage: float,
    current: float,
) -> None:

    if voltage <= 0:
        raise ValueError(
            "Invalid voltage"
        )

    if voltage > MAX_SAFE_VOLTAGE:
        raise ValueError(
            "Voltage exceeds test limit"
        )

    if current <= 0:
        raise ValueError(
            "Invalid current"
        )

    if current > MAX_TEST_CURRENT:
        raise ValueError(
            "Current exceeds test limit"
        )

The actual laboratory limits must come from the validated hardware/test configuration—not arbitrary constants in production code.

24. Full Surface validation architecture

At this point the roadmap has become a fairly coherent engineering platform:

                         SURFACE
                            │
       ┌────────────────────┼─────────────────────┐
       │                    │                     │
   Production             Services             Testing
       │                    │                     │
       ▼                    ▼                     ▼
 C++ Hardware          Rust Services        Python #30
       │                    │                     │
       ├── Power            ├── Security          ├── DUT
       ├── Thermal          ├── Radio             ├── Instruments
       ├── Display          ├── USB               ├── Test plans
       ├── Camera           ├── Storage            ├── Analysis
       ├── Graphics         ├── Crypto             └── Reports
       └── Performance      └── Sync
                            │
                            ▼
                       Windows

And the test system can validate almost everything we've built:

#1   Battery              ──► BAT
#2   Thermal              ──► THM
#3   Pen                 ──► PEN
#4   Touch               ──► TOUCH
#5   Scheduler            ──► CPU/NPU
#6   Display              ──► DISPLAY
#7   Camera               ──► CAMERA
#8   Connectivity         ──► RADIO
#9   Performance          ──► PERF
#10  Security             ──► SECURITY
#11  Rust security        ──► INTEGRITY
#12  AI                   ──► NPU
#16  Battery Julia        ──► BAT MODEL
#17  Radio                ──► WIFI/BT
#18  Photography          ──► CAMERA
#19  Audio                ──► AUDIO
#20  Cloud Sync           ──► NETWORK
#21  Device Services      ──► SERVICE
#22  Diagnostics          ──► DIAGNOSTICS
#23  Analytics            ──► REGRESSION
#24  Gaming               ──► GPU
#25  Web                   ──► UI
#26  Firmware              ──► MCU
#27  USB/TB               ──► USB/TB
#28  Local Index           ──► STORAGE
#29  Crypto                ──► SECURITY
#30  Automated Testing     ──► ALL OF THE ABOVE



