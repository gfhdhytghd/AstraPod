// SPDX-License-Identifier: GPL-2.0-or-later
#![cfg_attr(feature = "firmware", no_std)]
#![deny(unsafe_code)]
#[cfg(feature = "firmware")]
#[allow(unsafe_code)]
mod ffi;
pub mod service;
pub mod session;
pub mod wire;
#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests;
