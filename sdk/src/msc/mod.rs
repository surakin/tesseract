//! Home for the MSCs Tesseract implements directly as raw event JSON because
//! ruma and matrix-sdk lack them. One file per MSC, holding every key name
//! and JSON shape of that proposal; callers elsewhere only use its functions.

pub(crate) mod msc4553;
