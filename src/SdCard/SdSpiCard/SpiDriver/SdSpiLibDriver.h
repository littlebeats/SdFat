/**
 * Copyright (c) 2011-2025 Bill Greiman
 * This file is part of the SdFat library for SD memory cards.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/**
 * \file
 * \brief Class using only simple SPI library functions.
 */
#pragma once
#if defined(ARDUINO_ARCH_SAMD) && USE_SPI_ARRAY_TRANSFER == 2
extern "C" void lb_note_sd_spi_dma_transfer(
    uint8_t is_write,
    uint32_t elapsed_us,
    uint8_t timeout,
    uint32_t busych,
    uint32_t pendch);

inline bool lbSdSpiArrayTransferSAMD(
    SPIClass* spi,
    const void* txbuf,
    void* rxbuf,
    size_t count,
    uint8_t is_write) {
  constexpr uint32_t LB_SPI_DMA_TRANSFER_TIMEOUT_US = 10000;
  const uint32_t start_us = micros();
  spi->transfer(txbuf, rxbuf, count, false);
  while (spi->isBusy()) {
    const uint32_t elapsed_us = micros() - start_us;
    if (elapsed_us > LB_SPI_DMA_TRANSFER_TIMEOUT_US) {
      lb_note_sd_spi_dma_transfer(
          is_write,
          elapsed_us,
          1,
          DMAC->BUSYCH.reg,
          DMAC->PENDCH.reg);
      return false;
    }
  }
  lb_note_sd_spi_dma_transfer(is_write, micros() - start_us, 0, 0, 0);
  return true;
}
#endif  // defined(ARDUINO_ARCH_SAMD) && USE_SPI_ARRAY_TRANSFER == 2
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::activate() {
  m_spi->beginTransaction(m_spiSettings);
}
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::begin(SdSpiConfig spiConfig) {
  if (spiConfig.spiPort) {
    m_spi = spiConfig.spiPort;
#if defined(SDCARD_SPI) && defined(SDCARD_SS_PIN)
  } else if (spiConfig.csPin == SDCARD_SS_PIN) {
    m_spi = &SDCARD_SPI;
#endif  // defined(SDCARD_SPI) && defined(SDCARD_SS_PIN)
  } else {
    m_spi = &SPI;
  }
  if (!(spiConfig.options & USER_SPI_BEGIN)) {
    m_spi->begin();
  }
}
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::end() { m_spi->end(); }
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::deactivate() { m_spi->endTransaction(); }
//------------------------------------------------------------------------------
inline uint8_t SdSpiArduinoDriver::receive() { return m_spi->transfer(0XFF); }
//------------------------------------------------------------------------------
inline uint8_t SdSpiArduinoDriver::receive(uint8_t* buf, size_t count) {
#if USE_SPI_ARRAY_TRANSFER == 0
  for (size_t i = 0; i < count; i++) {
    buf[i] = m_spi->transfer(0XFF);
  }
#elif USE_SPI_ARRAY_TRANSFER == 1
  memset(buf, 0XFF, count);
  m_spi->transfer(buf, count);
#elif USE_SPI_ARRAY_TRANSFER < 4
#if defined(ARDUINO_ARCH_SAMD) && USE_SPI_ARRAY_TRANSFER == 2
  if (!lbSdSpiArrayTransferSAMD(m_spi, nullptr, buf, count, 0)) {
    return 1;
  }
#else
  m_spi->transfer(nullptr, buf, count);
#endif
#elif USE_SPI_ARRAY_TRANSFER == 4
  uint8_t txTmp[512];
  memset(txTmp, 0XFF, sizeof(txTmp));
  while (count) {
    size_t n = count <= sizeof(txTmp) ? count : sizeof(txTmp);
    m_spi->transfer(txTmp, buf, n);
    buf += n;
    count -= n;
  }
#else  // USE_SPI_ARRAY_TRANSFER == 0
#error invalid USE_SPI_ARRAY_TRANSFER
#endif  // USE_SPI_ARRAY_TRANSFER == 0
  return 0;
}
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::send(uint8_t data) { m_spi->transfer(data); }
//------------------------------------------------------------------------------
inline void SdSpiArduinoDriver::send(const uint8_t* buf, size_t count) {
#if USE_SPI_ARRAY_TRANSFER == 0
  for (size_t i = 0; i < count; i++) {
    m_spi->transfer(buf[i]);
  }
#elif USE_SPI_ARRAY_TRANSFER == 1
  uint8_t tmp[512];
  while (count > 0) {
    size_t n = count <= sizeof(tmp) ? count : sizeof(tmp);
    memcpy(tmp, buf, n);
    m_spi->transfer(tmp, n);
    count -= n;
    buf += n;
  }
#elif USE_SPI_ARRAY_TRANSFER == 2
  // Some systems do not allow const uint8_t*.
#if defined(ARDUINO_ARCH_SAMD)
  (void)lbSdSpiArrayTransferSAMD(
      m_spi,
      const_cast<uint8_t*>(buf),
      nullptr,
      count,
      1);
#else
  m_spi->transfer(const_cast<uint8_t*>(buf), nullptr, count);
#endif
#elif USE_SPI_ARRAY_TRANSFER < 5
  uint8_t rxTmp[512];
  while (count > 0) {
    size_t n = count <= sizeof(rxTmp) ? count : sizeof(rxTmp);
    // Some systems do not allow const uint8_t*.
    m_spi->transfer(const_cast<uint8_t*>(buf), rxTmp, n);
    buf += n;
    count -= n;
  }
#else  // if USE_SPI_ARRAY_TRANSFER == 0
#error invalid USE_SPI_ARRAY_TRANSFER
#endif  // USE_SPI_ARRAY_TRANSFER == 0
}
