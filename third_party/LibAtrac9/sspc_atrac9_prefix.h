// SecondSonPC: every external symbol of this LibAtrac9 copy gets the prefix SspcAt9_, so it never meets the
// identical symbols of the bridge's own LibAtrac9 (third_party/shadPS4/externals/LibAtrac9) at link time and the
// two decoders keep separate global tables. Force-included into each LibAtrac9 source (CMakeLists.txt, sspc_atrac9),
// included before libatrac9.h by SecondSonPC's AJM (src/runtime/audio/ajm.cpp): the public API only there.
#pragma once

#define Atrac9GetHandle SspcAt9_Atrac9GetHandle
#define Atrac9ReleaseHandle SspcAt9_Atrac9ReleaseHandle
#define Atrac9InitDecoder SspcAt9_Atrac9InitDecoder
#define Atrac9Decode SspcAt9_Atrac9Decode
#define Atrac9DecodeS32 SspcAt9_Atrac9DecodeS32
#define Atrac9DecodeF32 SspcAt9_Atrac9DecodeF32
#define Atrac9GetCodecInfo SspcAt9_Atrac9GetCodecInfo

#ifdef SSPC_ATRAC9_BUILD // the library's internal functions and tables
#define AlignPosition SspcAt9_AlignPosition
#define ApplyBandExtension SspcAt9_ApplyBandExtension
#define BandToQuantUnitCount SspcAt9_BandToQuantUnitCount
#define BexDataLengths SspcAt9_BexDataLengths
#define BexEncodedValueCounts SspcAt9_BexEncodedValueCounts
#define BexGroupInfo SspcAt9_BexGroupInfo
#define BitReverse32 SspcAt9_BitReverse32
#define CalculateMask SspcAt9_CalculateMask
#define CalculatePrecisions SspcAt9_CalculatePrecisions
#define ChannelConfigs SspcAt9_ChannelConfigs
#define Clamp16 SspcAt9_Clamp16
#define CosTables SspcAt9_CosTables
#define CreateGradient SspcAt9_CreateGradient
#define Decode SspcAt9_Decode
#define DecodeF32 SspcAt9_DecodeF32
#define DecodeHuffmanValues SspcAt9_DecodeHuffmanValues
#define DecodeS32 SspcAt9_DecodeS32
#define DequantizeSpectra SspcAt9_DequantizeSpectra
#define GenerateGradientCurves SspcAt9_GenerateGradientCurves
#define GetCodecInfo SspcAt9_GetCodecInfo
#define HuffmanScaleFactorsSigned SspcAt9_HuffmanScaleFactorsSigned
#define HuffmanScaleFactorsUnsigned SspcAt9_HuffmanScaleFactorsUnsigned
#define HuffmanSpectrum SspcAt9_HuffmanSpectrum
#define ImdctWindow SspcAt9_ImdctWindow
#define InitBitReaderCxt SspcAt9_InitBitReaderCxt
#define InitDecoder SspcAt9_InitDecoder
#define InitHuffmanCodebook SspcAt9_InitHuffmanCodebook
#define Max SspcAt9_Max
#define MaxBandCount SspcAt9_MaxBandCount
#define MaxExtensionBand SspcAt9_MaxExtensionBand
#define MaxHuffPrecision SspcAt9_MaxHuffPrecision
#define MdctWindow SspcAt9_MdctWindow
#define Min SspcAt9_Min
#define MinBandCount SspcAt9_MinBandCount
#define PcmFloatToF32 SspcAt9_PcmFloatToF32
#define PcmFloatToS32 SspcAt9_PcmFloatToS32
#define PcmFloatToShort SspcAt9_PcmFloatToShort
#define PeekInt SspcAt9_PeekInt
#define QuantUnitToCodebookIndex SspcAt9_QuantUnitToCodebookIndex
#define QuantUnitToCoeffCount SspcAt9_QuantUnitToCoeffCount
#define QuantUnitToCoeffIndex SspcAt9_QuantUnitToCoeffIndex
#define QuantizerFineStepSize SspcAt9_QuantizerFineStepSize
#define QuantizerInverseStepSize SspcAt9_QuantizerInverseStepSize
#define QuantizerStepSize SspcAt9_QuantizerStepSize
#define ReadHuffmanValue SspcAt9_ReadHuffmanValue
#define ReadInt SspcAt9_ReadInt
#define ReadOffsetBinary SspcAt9_ReadOffsetBinary
#define ReadScaleFactors SspcAt9_ReadScaleFactors
#define ReadSignedInt SspcAt9_ReadSignedInt
#define Round SspcAt9_Round
#define RunImdct SspcAt9_RunImdct
#define SampleRates SspcAt9_SampleRates
#define SamplingRateIndexToFrameSamplesPower SspcAt9_SamplingRateIndexToFrameSamplesPower
#define ScaleFactorWeights SspcAt9_ScaleFactorWeights
#define ScaleSpectrumBlock SspcAt9_ScaleSpectrumBlock
#define ShuffleTables SspcAt9_ShuffleTables
#define SignExtend32 SspcAt9_SignExtend32
#define SinTables SspcAt9_SinTables
#define SpectrumScale SspcAt9_SpectrumScale
#define UnpackFrame SspcAt9_UnpackFrame
#endif
