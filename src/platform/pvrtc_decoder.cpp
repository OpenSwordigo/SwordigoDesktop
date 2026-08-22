#include "pvrtc_decoder.h"
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <vector>

namespace pvr {

struct Pixel32
{
	uint8_t red, green, blue, alpha;
};

struct Pixel128S
{
	int32_t red, green, blue, alpha;
};

struct PVRTCWord
{
	uint32_t modulationData;
	uint32_t colorData;
};

struct PVRTCWordIndices
{
	int P[2], Q[2], R[2], S[2];
};

static Pixel32 getColorA(uint32_t colorData)
{
	Pixel32 color;

	// Opaque Color Mode - RGB 554
	if ((colorData & 0x8000) != 0)
	{
		color.red = static_cast<uint8_t>((colorData & 0x7c00) >> 10); // 5->5 bits
		color.green = static_cast<uint8_t>((colorData & 0x3e0) >> 5); // 5->5 bits
		color.blue = static_cast<uint8_t>(colorData & 0x1e) | ((colorData & 0x1e) >> 4); // 4->5 bits
		color.alpha = static_cast<uint8_t>(0xf); // 0->4 bits
	}
	// Transparent Color Mode - ARGB 3443
	else
	{
		color.red = static_cast<uint8_t>((colorData & 0xf00) >> 7) | ((colorData & 0xf00) >> 11); // 4->5 bits
		color.green = static_cast<uint8_t>((colorData & 0xf0) >> 3) | ((colorData & 0xf0) >> 7); // 4->5 bits
		color.blue = static_cast<uint8_t>((colorData & 0xe) << 1) | ((colorData & 0xe) >> 2); // 3->5 bits
		color.alpha = static_cast<uint8_t>((colorData & 0x7000) >> 11); // 3->4 bits - note 0 at right
	}

	return color;
}

static Pixel32 getColorB(uint32_t colorData)
{
	Pixel32 color;

	// Opaque Color Mode - RGB 555
	if (colorData & 0x80000000)
	{
		color.red = static_cast<uint8_t>((colorData & 0x7c000000) >> 26); // 5->5 bits
		color.green = static_cast<uint8_t>((colorData & 0x3e00000) >> 21); // 5->5 bits
		color.blue = static_cast<uint8_t>((colorData & 0x1f0000) >> 16); // 5->5 bits
		color.alpha = static_cast<uint8_t>(0xf); // 0 bits
	}
	// Transparent Color Mode - ARGB 3444
	else
	{
		color.red = static_cast<uint8_t>(((colorData & 0xf000000) >> 23) | ((colorData & 0xf000000) >> 27)); // 4->5 bits
		color.green = static_cast<uint8_t>(((colorData & 0xf00000) >> 19) | ((colorData & 0xf00000) >> 23)); // 4->5 bits
		color.blue = static_cast<uint8_t>(((colorData & 0xf0000) >> 15) | ((colorData & 0xf0000) >> 19)); // 4->5 bits
		color.alpha = static_cast<uint8_t>((colorData & 0x70000000) >> 27); // 3->4 bits - note 0 at right
	}

	return color;
}

static void interpolateColors(Pixel32 P, Pixel32 Q, Pixel32 R, Pixel32 S, Pixel128S* pPixel, uint8_t bpp)
{
	uint32_t wordWidth = 4;
	uint32_t wordHeight = 4;
	if (bpp == 2) { wordWidth = 8; }

	// Convert to int 32.
	Pixel128S hP = { static_cast<int32_t>(P.red), static_cast<int32_t>(P.green), static_cast<int32_t>(P.blue), static_cast<int32_t>(P.alpha) };
	Pixel128S hQ = { static_cast<int32_t>(Q.red), static_cast<int32_t>(Q.green), static_cast<int32_t>(Q.blue), static_cast<int32_t>(Q.alpha) };
	Pixel128S hR = { static_cast<int32_t>(R.red), static_cast<int32_t>(R.green), static_cast<int32_t>(R.blue), static_cast<int32_t>(R.alpha) };
	Pixel128S hS = { static_cast<int32_t>(S.red), static_cast<int32_t>(S.green), static_cast<int32_t>(S.blue), static_cast<int32_t>(S.alpha) };

	// Get vectors.
	Pixel128S QminusP = { hQ.red - hP.red, hQ.green - hP.green, hQ.blue - hP.blue, hQ.alpha - hP.alpha };
	Pixel128S SminusR = { hS.red - hR.red, hS.green - hR.green, hS.blue - hR.blue, hS.alpha - hR.alpha };

	// Multiply colors.
	hP.red *= wordWidth;
	hP.green *= wordWidth;
	hP.blue *= wordWidth;
	hP.alpha *= wordWidth;
	hR.red *= wordWidth;
	hR.green *= wordWidth;
	hR.blue *= wordWidth;
	hR.alpha *= wordWidth;

	if (bpp == 2)
	{
		// Loop through pixels to achieve results.
		for (uint32_t x = 0; x < wordWidth; x++)
		{
			Pixel128S result = { 4 * hP.red, 4 * hP.green, 4 * hP.blue, 4 * hP.alpha };
			Pixel128S dY = { hR.red - hP.red, hR.green - hP.green, hR.blue - hP.blue, hR.alpha - hP.alpha };

			for (uint32_t y = 0; y < wordHeight; y++)
			{
				pPixel[y * wordWidth + x].red = static_cast<int32_t>((result.red >> 7) + (result.red >> 2));
				pPixel[y * wordWidth + x].green = static_cast<int32_t>((result.green >> 7) + (result.green >> 2));
				pPixel[y * wordWidth + x].blue = static_cast<int32_t>((result.blue >> 7) + (result.blue >> 2));
				pPixel[y * wordWidth + x].alpha = static_cast<int32_t>((result.alpha >> 5) + (result.alpha >> 1));

				result.red += dY.red;
				result.green += dY.green;
				result.blue += dY.blue;
				result.alpha += dY.alpha;
			}

			hP.red += QminusP.red;
			hP.green += QminusP.green;
			hP.blue += QminusP.blue;
			hP.alpha += QminusP.alpha;

			hR.red += SminusR.red;
			hR.green += SminusR.green;
			hR.blue += SminusR.blue;
			hR.alpha += SminusR.alpha;
		}
	}
	else
	{
		// Loop through pixels to achieve results.
		for (uint32_t y = 0; y < wordHeight; y++)
		{
			Pixel128S result = { 4 * hP.red, 4 * hP.green, 4 * hP.blue, 4 * hP.alpha };
			Pixel128S dY = { hR.red - hP.red, hR.green - hP.green, hR.blue - hP.blue, hR.alpha - hP.alpha };

			for (uint32_t x = 0; x < wordWidth; x++)
			{
				pPixel[y * wordWidth + x].red = static_cast<int32_t>((result.red >> 6) + (result.red >> 1));
				pPixel[y * wordWidth + x].green = static_cast<int32_t>((result.green >> 6) + (result.green >> 1));
				pPixel[y * wordWidth + x].blue = static_cast<int32_t>((result.blue >> 6) + (result.blue >> 1));
				pPixel[y * wordWidth + x].alpha = static_cast<int32_t>((result.alpha >> 4) + (result.alpha));

				result.red += dY.red;
				result.green += dY.green;
				result.blue += dY.blue;
				result.alpha += dY.alpha;
			}

			hP.red += QminusP.red;
			hP.green += QminusP.green;
			hP.blue += QminusP.blue;
			hP.alpha += QminusP.alpha;

			hR.red += SminusR.red;
			hR.green += SminusR.green;
			hR.blue += SminusR.blue;
			hR.alpha += SminusR.alpha;
		}
	}
}

static void unpackModulations(const PVRTCWord& word, int32_t offsetX, int32_t offsetY, int32_t modulationValues[16][8], int32_t modulationModes[16][8], uint8_t bpp)
{
	uint32_t WordModMode = word.colorData & 0x1;
	uint32_t ModulationBits = word.modulationData;

	// Unpack differently depending on 2bpp or 4bpp modes.
	if (bpp == 2)
	{
		if (WordModMode)
		{
			// determine which of the three modes are in use:

			// If this is the either the H-only or V-only interpolation mode...
			if (ModulationBits & 0x1)
			{
				// look at the "LSB" for the "centre" (V=2,H=4) texel. Its LSB is now
				// actually used to indicate whether it's the H-only mode or the V-only...

				// The centre texel data is the at (y==2, x==4) and so its LSB is at bit 20.
				if (ModulationBits & (0x1 << 20))
				{
					// This is the V-only mode
					WordModMode = 3;
				}
				else
				{
					// This is the H-only mode
					WordModMode = 2;
				}

				// Create an extra bit for the centre pixel so that it looks like
				// we have 2 actual bits for this texel. It makes later coding much easier.
				if (ModulationBits & (0x1 << 21))
				{
					// set it to produce code for 1.0
					ModulationBits |= (0x1 << 20);
				}
				else
				{
					// clear it to produce 0.0 code
					ModulationBits &= ~(0x1 << 20);
				}
			} // end if H-Only or V-Only interpolation mode was chosen

			if (ModulationBits & 0x2) { ModulationBits |= 0x1; /*set it*/ }
			else
			{
				ModulationBits &= ~0x1; /*clear it*/
			}

			// run through all the pixels in the block. Note we can now treat all the
			// "stored" values as if they have 2bits (even when they didn't!)
			for (uint8_t y = 0; y < 4; y++)
			{
				for (uint8_t x = 0; x < 8; x++)
				{
					modulationModes[static_cast<uint32_t>(x + offsetX)][static_cast<uint32_t>(y + offsetY)] = WordModMode;

					// if this is a stored value...
					if (((x ^ y) & 1) == 0) {modulationValues[static_cast<uint32_t>(x + offsetX)][static_cast<uint32_t>(y + offsetY)] = ModulationBits & 3;
						ModulationBits >>= 2;
					}
				}
			} // end for y
		}
		// else if direct encoded 2bit mode - i.e. 1 mode bit per pixel
		else
		{
			for (uint8_t y = 0; y < 4; y++)
			{
				for (uint8_t x = 0; x < 8; x++)
				{
					modulationModes[static_cast<uint32_t>(x + offsetX)][static_cast<uint32_t>(y + offsetY)] = WordModMode;

					/*
					// double the bits so 0=> 00, and 1=>11
					*/
					if (ModulationBits & 1) { modulationValues[static_cast<uint32_t>(x + offsetX)][static_cast<uint32_t>(y + offsetY)] = 0x3; }
					else
					{
						modulationValues[static_cast<uint32_t>(x + offsetX)][static_cast<uint32_t>(y + offsetY)] = 0x0;
					}
					ModulationBits >>= 1;
				}
			} // end for y
		}
	}
	else
	{
		// Much simpler than the 2bpp decompression, only two modes, so the n/8 values are set directly.
		// run through all the pixels in the word.
		if (WordModMode)
		{
			for (uint8_t y = 0; y < 4; y++)
			{
				for (uint8_t x = 0; x < 4; x++)
				{
					modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] = ModulationBits & 3;
					// if (modulationValues==0) {}. We don't need to check 0, 0 = 0/8.
					if (modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] == 1)
					{ modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] = 4; }
					else if (modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] == 2)
					{
						modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] = 14; //+10 tells the decompressor to punch through alpha.
					}
					else if (modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] == 3)
					{
						modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] = 8;
					}
					ModulationBits >>= 2;
				} // end for x
			} // end for y
		}
		else
		{
			for (uint8_t y = 0; y < 4; y++)
			{
				for (uint8_t x = 0; x < 4; x++)
				{
					modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] = ModulationBits & 3;
					modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] *= 3;
					if (modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] > 3)
					{ modulationValues[static_cast<uint32_t>(y + offsetY)][static_cast<uint32_t>(x + offsetX)] -= 1; }
					ModulationBits >>= 2;
				} // end for x
			} // end for y
		}
	}
}

static int32_t getModulationValues(int32_t modulationValues[16][8], int32_t modulationModes[16][8], uint32_t xPos, uint32_t yPos, uint8_t bpp)
{
	if (bpp == 2)
	{
		const int32_t RepVals0[4] = { 0, 3, 5, 8 };

		// extract the modulation value. If a simple encoding
		if (modulationModes[xPos][yPos] == 0) { return RepVals0[modulationValues[xPos][yPos]]; }
		else
		{
			// if this is a stored value
			if (((xPos ^ yPos) & 1) == 0) { return RepVals0[modulationValues[xPos][yPos]]; }

			// else average from the neighbours
			// if H&V interpolation...
			else if (modulationModes[xPos][yPos] == 1)
			{
				return (RepVals0[modulationValues[xPos][yPos - 1]] + RepVals0[modulationValues[xPos][yPos + 1]] + RepVals0[modulationValues[xPos - 1][yPos]] +
						   RepVals0[modulationValues[xPos + 1][yPos]] + 2) /
					4;
			}
			// else if H-Only
			else if (modulationModes[xPos][yPos] == 2)
			{
				return (RepVals0[modulationValues[xPos - 1][yPos]] + RepVals0[modulationValues[xPos + 1][yPos]] + 1) / 2;
			}
			// else it's V-Only
			else
			{
				return (RepVals0[modulationValues[xPos][yPos - 1]] + RepVals0[modulationValues[xPos][yPos + 1]] + 1) / 2;
			}
		}
	}
	else if (bpp == 4)
	{
		return modulationValues[xPos][yPos];
	}

	return 0;
}

static void pvrtcGetDecompressedPixels(const PVRTCWord& P, const PVRTCWord& Q, const PVRTCWord& R, const PVRTCWord& S, Pixel32* pColorData, uint8_t bpp)
{
	int32_t modulationValues[16][8];
	int32_t modulationModes[16][8];
	Pixel128S upscaledColorA[32];
	Pixel128S upscaledColorB[32];

	uint32_t wordWidth = 4;
	uint32_t wordHeight = 4;
	if (bpp == 2) { wordWidth = 8; }

	unpackModulations(P, 0, 0, modulationValues, modulationModes, bpp);
	unpackModulations(Q, wordWidth, 0, modulationValues, modulationModes, bpp);
	unpackModulations(R, 0, wordHeight, modulationValues, modulationModes, bpp);
	unpackModulations(S, wordWidth, wordHeight, modulationValues, modulationModes, bpp);

	interpolateColors(getColorA(P.colorData), getColorA(Q.colorData), getColorA(R.colorData), getColorA(S.colorData), upscaledColorA, bpp);
	interpolateColors(getColorB(P.colorData), getColorB(Q.colorData), getColorB(R.colorData), getColorB(S.colorData), upscaledColorB, bpp);

	for (uint32_t y = 0; y < wordHeight; y++)
	{
		for (uint32_t x = 0; x < wordWidth; x++)
		{
			int32_t mod = getModulationValues(modulationValues, modulationModes, x + wordWidth / 2, y + wordHeight / 2, bpp);
			bool punchthroughAlpha = false;
			if (mod > 10)
			{
				punchthroughAlpha = true;
				mod -= 10;
			}

			Pixel128S result;
			result.red = (upscaledColorA[y * wordWidth + x].red * (8 - mod) + upscaledColorB[y * wordWidth + x].red * mod) / 8;
			result.green = (upscaledColorA[y * wordWidth + x].green * (8 - mod) + upscaledColorB[y * wordWidth + x].green * mod) / 8;
			result.blue = (upscaledColorA[y * wordWidth + x].blue * (8 - mod) + upscaledColorB[y * wordWidth + x].blue * mod) / 8;
			if (punchthroughAlpha) { result.alpha = 0; }
			else
			{
				result.alpha = (upscaledColorA[y * wordWidth + x].alpha * (8 - mod) + upscaledColorB[y * wordWidth + x].alpha * mod) / 8;
			}

			if (bpp == 2)
			{
				pColorData[y * wordWidth + x].red = static_cast<uint8_t>(result.red);
				pColorData[y * wordWidth + x].green = static_cast<uint8_t>(result.green);
				pColorData[y * wordWidth + x].blue = static_cast<uint8_t>(result.blue);
				pColorData[y * wordWidth + x].alpha = static_cast<uint8_t>(result.alpha);
			}
			else if (bpp == 4)
			{
				pColorData[y + x * wordHeight].red = static_cast<uint8_t>(result.red);
				pColorData[y + x * wordHeight].green = static_cast<uint8_t>(result.green);
				pColorData[y + x * wordHeight].blue = static_cast<uint8_t>(result.blue);
				pColorData[y + x * wordHeight].alpha = static_cast<uint8_t>(result.alpha);
			}
		}
	}
}

static uint32_t wrapWordIndex(uint32_t numWords, int word) { return ((word + numWords) % numWords); }

static bool isPowerOf2(uint32_t input)
{
	uint32_t minus1;
	if (!input) { return 0; }
	minus1 = input - 1;
	return ((input | minus1) == (input ^ minus1));
}

static uint32_t TwiddleUV(uint32_t XSize, uint32_t YSize, uint32_t XPos, uint32_t YPos)
{
	uint32_t MinDimension = XSize;
	uint32_t MaxValue = YPos;
	uint32_t Twiddled = 0;
	uint32_t SrcBitPos = 1;
	uint32_t DstBitPos = 1;
	int ShiftCount = 0;

	assert(YPos < YSize);
	assert(XPos < XSize);
	assert(isPowerOf2(YSize));
	assert(isPowerOf2(XSize));

	if (YSize < XSize)
	{
		MinDimension = YSize;
		MaxValue = XPos;
	}

	while (SrcBitPos < MinDimension)
	{
		if (YPos & SrcBitPos) { Twiddled |= DstBitPos; }
		if (XPos & SrcBitPos) { Twiddled |= (DstBitPos << 1); }
		SrcBitPos <<= 1;
		DstBitPos <<= 2;
		ShiftCount += 1;
	}

	MaxValue >>= ShiftCount;
	Twiddled |= (MaxValue << (2 * ShiftCount));
	return Twiddled;
}

static void mapDecompressedData(Pixel32* pOutput, uint32_t width, const Pixel32* pWord, const PVRTCWordIndices& words, uint8_t bpp)
{
	uint32_t wordWidth = 4;
	uint32_t wordHeight = 4;
	if (bpp == 2) { wordWidth = 8; }

	for (uint32_t y = 0; y < wordHeight / 2; y++)
	{
		for (uint32_t x = 0; x < wordWidth / 2; x++)
		{
			pOutput[(((words.P[1] * wordHeight) + y + wordHeight / 2) * width + words.P[0] * wordWidth + x + wordWidth / 2)] = pWord[y * wordWidth + x]; // map P
			pOutput[(((words.Q[1] * wordHeight) + y + wordHeight / 2) * width + words.Q[0] * wordWidth + x)] = pWord[y * wordWidth + x + wordWidth / 2]; // map Q
			pOutput[(((words.R[1] * wordHeight) + y) * width + words.R[0] * wordWidth + x + wordWidth / 2)] = pWord[(y + wordHeight / 2) * wordWidth + x]; // map R
			pOutput[(((words.S[1] * wordHeight) + y) * width + words.S[0] * wordWidth + x)] = pWord[(y + wordHeight / 2) * wordWidth + x + wordWidth / 2]; // map S
		}
	}
}

static uint32_t pvrtcDecompress(uint8_t* pCompressedData, Pixel32* pDecompressedData, uint32_t width, uint32_t height, uint8_t bpp)
{
	uint32_t wordWidth = 4;
	uint32_t wordHeight = 4;
	if (bpp == 2) { wordWidth = 8; }

	uint32_t* pWordMembers = (uint32_t*)pCompressedData;
	Pixel32* pOutData = pDecompressedData;

	int i32NumXWords = static_cast<int>(width / wordWidth);
	int i32NumYWords = static_cast<int>(height / wordHeight);

	PVRTCWordIndices indices;
	std::vector<Pixel32> pPixels(wordWidth * wordHeight);

	for (int32_t wordY = -1; wordY < i32NumYWords - 1; wordY++)
	{
		for (int32_t wordX = -1; wordX < i32NumXWords - 1; wordX++)
		{
			indices.P[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX));
			indices.P[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY));
			indices.Q[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX + 1));
			indices.Q[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY));
			indices.R[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX));
			indices.R[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY + 1));
			indices.S[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX + 1));
			indices.S[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY + 1));

			uint32_t WordOffsets[4] = {
				TwiddleUV(i32NumXWords, i32NumYWords, indices.P[0], indices.P[1]) * 2,
				TwiddleUV(i32NumXWords, i32NumYWords, indices.Q[0], indices.Q[1]) * 2,
				TwiddleUV(i32NumXWords, i32NumYWords, indices.R[0], indices.R[1]) * 2,
				TwiddleUV(i32NumXWords, i32NumYWords, indices.S[0], indices.S[1]) * 2,
			};

			PVRTCWord P, Q, R, S;
			P.colorData = static_cast<uint32_t>(pWordMembers[WordOffsets[0] + 1]);
			P.modulationData = static_cast<uint32_t>(pWordMembers[WordOffsets[0]]);
			Q.colorData = static_cast<uint32_t>(pWordMembers[WordOffsets[1] + 1]);
			Q.modulationData = static_cast<uint32_t>(pWordMembers[WordOffsets[1]]);
			R.colorData = static_cast<uint32_t>(pWordMembers[WordOffsets[2] + 1]);
			R.modulationData = static_cast<uint32_t>(pWordMembers[WordOffsets[2]]);
			S.colorData = static_cast<uint32_t>(pWordMembers[WordOffsets[3] + 1]);
			S.modulationData = static_cast<uint32_t>(pWordMembers[WordOffsets[3]]);

			pvrtcGetDecompressedPixels(P, Q, R, S, pPixels.data(), bpp);
			mapDecompressedData(pOutData, width, pPixels.data(), indices, bpp);
		}
	}

	return width * height / static_cast<uint32_t>((wordWidth / 2));
}

uint32_t PVRTDecompressPVRTC(const void* pCompressedData, uint32_t Do2bitMode, uint32_t XDim, uint32_t YDim, uint8_t* pResultImage)
{
	Pixel32* pDecompressedData = (Pixel32*)pResultImage;

	uint32_t XTrueDim = std::max(XDim, ((Do2bitMode == 1u) ? 16u : 8u));
	uint32_t YTrueDim = std::max(YDim, 8u);

	if (XTrueDim != XDim || YTrueDim != YDim) { pDecompressedData = new Pixel32[XTrueDim * YTrueDim]; }

	uint32_t retval = pvrtcDecompress((uint8_t*)pCompressedData, pDecompressedData, XTrueDim, YTrueDim, uint8_t(Do2bitMode == 1 ? 2 : 4));

	if (XTrueDim != XDim || YTrueDim != YDim)
	{
		for (uint32_t x = 0; x < XDim; ++x)
		{
			for (uint32_t y = 0; y < YDim; ++y) { ((Pixel32*)pResultImage)[x + y * XDim] = pDecompressedData[x + y * XTrueDim]; }
		}
		delete[] pDecompressedData;
	}
	return retval;
}

static inline int etc1_clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static const int etc1_modifiers[8][2] = {
	{  2,   8}, {  5,  17}, {  9,  29}, { 13,  42},
	{ 18,  56}, { 24,  71}, { 33,  92}, { 47, 127}
};

static void decode_etc1_block(const uint8_t* src, uint8_t* dst, int dst_stride) {
	uint64_t block = 0;
	for (int i = 0; i < 8; i++)
		block = (block << 8) | src[i];
	
	int diff  = (block >> 33) & 1;
	int flip  = (block >> 32) & 1;
	int table1 = (block >> 37) & 7;
	int table2 = (block >> 34) & 7;
	
	int r1, g1, b1, r2, g2, b2;
	
	if (diff == 0) {
		int rr1 = (block >> 60) & 0xF; r1 = (rr1 << 4) | rr1;
		int rr2 = (block >> 56) & 0xF; r2 = (rr2 << 4) | rr2;
		int gg1 = (block >> 52) & 0xF; g1 = (gg1 << 4) | gg1;
		int gg2 = (block >> 48) & 0xF; g2 = (gg2 << 4) | gg2;
		int bb1 = (block >> 44) & 0xF; b1 = (bb1 << 4) | bb1;
		int bb2 = (block >> 40) & 0xF; b2 = (bb2 << 4) | bb2;
	} else {
		int r = (block >> 59) & 0x1F;
		int dr = (block >> 56) & 0x7; if (dr > 3) dr -= 8;
		int g = (block >> 51) & 0x1F;
		int dg = (block >> 48) & 0x7; if (dg > 3) dg -= 8;
		int b = (block >> 43) & 0x1F;
		int db = (block >> 40) & 0x7; if (db > 3) db -= 8;
		
		r1 = (r << 3) | (r >> 2);
		int r2v = r + dr; r2 = (r2v << 3) | (r2v >> 2);
		g1 = (g << 3) | (g >> 2);
		int g2v = g + dg; g2 = (g2v << 3) | (g2v >> 2);
		b1 = (b << 3) | (b >> 2);
		int dbv = b + db; b2 = (dbv << 3) | (dbv >> 2);
	}
	
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			int pixel_idx = col * 4 + row;
			int msb = (block >> (pixel_idx + 16)) & 1;
			int lsb = (block >> pixel_idx) & 1;
			int sub = (flip == 0) ? ((col >= 2) ? 1 : 0) : ((row >= 2) ? 1 : 0);
			int rb = sub ? r2 : r1;
			int gb = sub ? g2 : g1;
			int bb = sub ? b2 : b1;
			int table = sub ? table2 : table1;
			int mod = etc1_modifiers[table][lsb];
			if (msb) mod = -mod;
			
			uint8_t* pixel = dst + row * dst_stride + col * 4;
			pixel[0] = etc1_clamp(rb + mod);
			pixel[1] = etc1_clamp(gb + mod);
			pixel[2] = etc1_clamp(bb + mod);
			pixel[3] = 255;
		}
	}
}

uint32_t PVRTDecompressETC(const void* srcData, uint32_t width, uint32_t height, uint8_t* dstData, uint32_t format) {
	const uint8_t* src = (const uint8_t*)srcData;
	uint8_t* dst = dstData;
	int block_w = (width + 3) / 4;
	int block_h = (height + 3) / 4;
	
	for (int by = 0; by < block_h; ++by) {
		for (int bx = 0; bx < block_w; ++bx) {
			uint8_t block_rgba[4 * 4 * 4];
			decode_etc1_block(src + (by * block_w + bx) * 8, block_rgba, 16);
			for (int y = 0; y < 4 && (by * 4 + y) < (int)height; ++y) {
				for (int x = 0; x < 4 && (bx * 4 + x) < (int)width; ++x) {
					memcpy(dst + ((by * 4 + y) * width + (bx * 4 + x)) * 4, block_rgba + y * 16 + x * 4, 4);
				}
			}
		}
	}
	return width * height * 4;
}

static void decompress_dxt1_block(const uint8_t* block, uint8_t* rgba, int stride) {
	uint16_t color0 = block[0] | (block[1] << 8);
	uint16_t color1 = block[2] | (block[3] << 8);
	uint32_t bits = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);

	uint8_t r0 = ((color0 >> 11) & 31) << 3; r0 |= r0 >> 5;
	uint8_t g0 = ((color0 >> 5) & 63) << 2;  g0 |= g0 >> 6;
	uint8_t b0 = (color0 & 31) << 3;         b0 |= b0 >> 5;

	uint8_t r1 = ((color1 >> 11) & 31) << 3; r1 |= r1 >> 5;
	uint8_t g1 = ((color1 >> 5) & 63) << 2;  g1 |= g1 >> 6;
	uint8_t b1 = (color1 & 31) << 3;         b1 |= b1 >> 5;

	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			uint8_t code = (bits >> (2 * (y * 4 + x))) & 3;
			uint8_t r = 0, g = 0, b = 0, a = 255;
			if (code == 0) {
				r = r0; g = g0; b = b0;
			} else if (code == 1) {
				r = r1; g = g1; b = b1;
			} else if (code == 2) {
				if (color0 > color1) {
					r = (2 * r0 + r1) / 3;
					g = (2 * g0 + g1) / 3;
					b = (2 * b0 + b1) / 3;
				} else {
					r = (r0 + r1) / 2;
					g = (g0 + g1) / 2;
					b = (b0 + b1) / 2;
				}
			} else if (code == 3) {
				if (color0 > color1) {
					r = (r0 + 2 * r1) / 3;
					g = (g0 + 2 * g1) / 3;
					b = (b0 + 2 * b1) / 3;
				} else {
					r = g = b = a = 0;
				}
			}
			uint8_t* pixel = rgba + y * stride + x * 4;
			pixel[0] = r;
			pixel[1] = g;
			pixel[2] = b;
			pixel[3] = a;
		}
	}
}

static void decompress_dxt3_block(const uint8_t* block, uint8_t* rgba, int stride) {
	decompress_dxt1_block(block + 8, rgba, stride);
	for (int y = 0; y < 4; ++y) {
		uint16_t alpha_row = block[y * 2] | (block[y * 2 + 1] << 8);
		for (int x = 0; x < 4; ++x) {
			uint8_t a = (alpha_row >> (4 * x)) & 0xF;
			a = (a << 4) | a;
			rgba[y * stride + x * 4 + 3] = a;
		}
	}
}

static void decompress_dxt5_block(const uint8_t* block, uint8_t* rgba, int stride) {
	decompress_dxt1_block(block + 8, rgba, stride);
	
	uint8_t alpha0 = block[0];
	uint8_t alpha1 = block[1];
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i) {
		bits |= ((uint64_t)block[2 + i]) << (8 * i);
	}
	
	uint8_t alphas[8];
	alphas[0] = alpha0;
	alphas[1] = alpha1;
	if (alpha0 > alpha1) {
		alphas[2] = (6 * alpha0 + 1 * alpha1) / 7;
		alphas[3] = (5 * alpha0 + 2 * alpha1) / 7;
		alphas[4] = (4 * alpha0 + 3 * alpha1) / 7;
		alphas[5] = (3 * alpha0 + 4 * alpha1) / 7;
		alphas[6] = (2 * alpha0 + 5 * alpha1) / 7;
		alphas[7] = (1 * alpha0 + 6 * alpha1) / 7;
	} else {
		alphas[2] = (4 * alpha0 + 1 * alpha1) / 5;
		alphas[3] = (3 * alpha0 + 2 * alpha1) / 5;
		alphas[4] = (2 * alpha0 + 3 * alpha1) / 5;
		alphas[5] = (1 * alpha0 + 4 * alpha1) / 5;
		alphas[6] = 0;
		alphas[7] = 255;
	}
	
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			int bit_idx = 3 * (y * 4 + x);
			uint8_t code = (bits >> bit_idx) & 7;
			rgba[y * stride + x * 4 + 3] = alphas[code];
		}
	}
}

uint32_t PVRTDecompressDXT(const void* srcData, uint32_t width, uint32_t height, uint8_t* dstData, uint32_t format) {
	const uint8_t* src = (const uint8_t*)srcData;
	uint8_t* dst = dstData;
	int block_w = (width + 3) / 4;
	int block_h = (height + 3) / 4;
	int block_size = (format == 1) ? 8 : 16;
	
	for (int by = 0; by < block_h; ++by) {
		for (int bx = 0; bx < block_w; ++bx) {
			uint8_t block_rgba[4 * 4 * 4];
			if (format == 1) {
				decompress_dxt1_block(src + (by * block_w + bx) * block_size, block_rgba, 16);
			} else if (format == 3) {
				decompress_dxt3_block(src + (by * block_w + bx) * block_size, block_rgba, 16);
			} else if (format == 5) {
				decompress_dxt5_block(src + (by * block_w + bx) * block_size, block_rgba, 16);
			}
			
			for (int y = 0; y < 4 && (by * 4 + y) < (int)height; ++y) {
				for (int x = 0; x < 4 && (bx * 4 + x) < (int)width; ++x) {
					memcpy(dst + ((by * 4 + y) * width + (bx * 4 + x)) * 4, block_rgba + y * 16 + x * 4, 4);
				}
			}
		}
	}
	return width * height * 4;
}

bool PVRTDecodeUncompressed(const void* srcData, uint32_t width, uint32_t height,
                            char c0, char c1, char c2, char c3,
                            uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3,
                            uint8_t* dstData) {
	const uint8_t* src = (const uint8_t*)srcData;
	uint8_t* dst = dstData;
	
	uint32_t total_bits = d0 + d1 + d2 + d3;
	uint32_t bytes_per_pixel = (total_bits + 7) / 8;
	if (bytes_per_pixel == 0) return false;
	
	uint32_t shift0 = 0;
	uint32_t shift1 = d0;
	uint32_t shift2 = d0 + d1;
	uint32_t shift3 = d0 + d1 + d2;
	
	for (uint32_t i = 0; i < width * height; ++i) {
		uint64_t val = 0;
		for (uint32_t b = 0; b < bytes_per_pixel; ++b) {
			val |= ((uint64_t)src[b]) << (8 * b);
		}
		src += bytes_per_pixel;
		
		uint8_t r = 0, g = 0, b = 0, a = 255;
		
		auto get_channel = [&](char chan, uint8_t depth, uint32_t shift) {
			if (depth == 0 || chan == 0) return;
			uint64_t mask = (1ULL << depth) - 1;
			uint32_t raw = (uint32_t)((val >> shift) & mask);
			// Scale to 8-bit
			uint8_t scaled = (uint8_t)((raw * 255 + mask / 2) / mask);
			
			if (chan == 'r') r = scaled;
			else if (chan == 'g') g = scaled;
			else if (chan == 'b') b = scaled;
			else if (chan == 'a') a = scaled;
			else if (chan == 'l' || chan == 'i') {
				r = g = b = scaled;
			}
		};
		
		get_channel(c0, d0, shift0);
		get_channel(c1, d1, shift1);
		get_channel(c2, d2, shift2);
		get_channel(c3, d3, shift3);
		
		*dst++ = r;
		*dst++ = g;
		*dst++ = b;
		*dst++ = a;
	}
	return true;
}

int ParsePVRv3Format(uint64_t pixel_format, uint32_t& gl_format, uint32_t& gl_type, int& bpp,
                     char& c0, char& c1, char& c2, char& c3,
                     uint8_t& d0, uint8_t& d1, uint8_t& d2, uint8_t& d3) {
	uint32_t fmt_high = pixel_format >> 32;
	if (fmt_high == 0) {
		// Compressed format
		uint32_t fmt_low = pixel_format & 0xFFFFFFFF;
		gl_type = 0x1401; // GL_UNSIGNED_BYTE
		gl_format = 0x1908; // GL_RGBA
		bpp = 4;
		
		if (fmt_low == 6) {
			return 1; // ETC1
		} else if (fmt_low == 0 || fmt_low == 1) {
			bpp = 2; // PVRTC 2bpp
			return 2;
		} else if (fmt_low == 2 || fmt_low == 3) {
			bpp = 4; // PVRTC 4bpp
			return 3;
		} else if (fmt_low == 7) {
			return 4; // DXT1
		} else if (fmt_low == 9) {
			return 5; // DXT3
		} else if (fmt_low == 11) {
			return 6; // DXT5
		}
		return -1; // Unsupported compressed format
	}
	
	// Uncompressed
	c0 = (char)(pixel_format & 0xFF);
	c1 = (char)((pixel_format >> 8) & 0xFF);
	c2 = (char)((pixel_format >> 16) & 0xFF);
	c3 = (char)((pixel_format >> 24) & 0xFF);
	d0 = (uint8_t)((pixel_format >> 32) & 0xFF);
	d1 = (uint8_t)((pixel_format >> 40) & 0xFF);
	d2 = (uint8_t)((pixel_format >> 48) & 0xFF);
	d3 = (uint8_t)((pixel_format >> 56) & 0xFF);
	
	bpp = (d0 + d1 + d2 + d3 + 7) / 8;
	gl_type = 0x1401; // GL_UNSIGNED_BYTE
	
	if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) {
		gl_format = 0x1908; // GL_RGBA
		return 10;
	}
	if (c0 == 'b' && c1 == 'g' && c2 == 'r' && c3 == 'a' && d0 == 8 && d1 == 8 && d2 == 8 && d3 == 8) {
		gl_format = 0x80E1; // GL_BGRA
		return 10;
	}
	if (c0 == 'l' && c1 == 'a' && d0 == 8 && d1 == 8) {
		gl_format = 0x190A; // GL_LUMINANCE_ALPHA
		return 10;
	}
	if (c0 == 'l' && d0 == 8) {
		gl_format = 0x1909; // GL_LUMINANCE
		return 10;
	}
	if (c0 == 'a' && d0 == 8) {
		gl_format = 0x1906; // GL_ALPHA
		return 10;
	}
	if (c0 == 'r' && c1 == 'g' && c2 == 'b' && d0 == 5 && d1 == 6 && d2 == 5) {
		gl_format = 0x1907; // GL_RGB
		gl_type = 0x8363; // GL_UNSIGNED_SHORT_5_6_5
		return 10;
	}
	if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 4 && d1 == 4 && d2 == 4 && d3 == 4) {
		gl_format = 0x1908; // GL_RGBA
		gl_type = 0x8033; // GL_UNSIGNED_SHORT_4_4_4_4
		return 10;
	}
	if (c0 == 'r' && c1 == 'g' && c2 == 'b' && c3 == 'a' && d0 == 5 && d1 == 5 && d2 == 5 && d3 == 1) {
		gl_format = 0x1908; // GL_RGBA
		gl_type = 0x8034; // GL_UNSIGNED_SHORT_5_5_5_1
		return 10;
	}
	
	// Default fallback
	gl_format = 0x1908; // GL_RGBA
	return 10;
}

int ParsePVRv2Format(uint32_t flags, uint32_t& gl_format, uint32_t& gl_type, int& bpp,
                     char& c0, char& c1, char& c2, char& c3,
                     uint8_t& d0, uint8_t& d1, uint8_t& d2, uint8_t& d3) {
	uint32_t fmt = flags & 0xFF;
	gl_type = 0x1401; // GL_UNSIGNED_BYTE
	
	if (fmt == 0x36 || fmt == 0x06) {
		gl_format = 0x1908; bpp = 4;
		return 1; // ETC1
	} else if (fmt == 0x0c || fmt == 0x18) {
		gl_format = 0x1908; bpp = 2;
		return 2; // PVRTC 2bpp
	} else if (fmt == 0x0d || fmt == 0x19) {
		gl_format = 0x1908; bpp = 4;
		return 3; // PVRTC 4bpp
	} else if (fmt == 0x20) {
		gl_format = 0x1908; bpp = 4;
		return 4; // DXT1
	} else if (fmt == 0x22) {
		gl_format = 0x1908; bpp = 4;
		return 5; // DXT3
	} else if (fmt == 0x24) {
		gl_format = 0x1908; bpp = 4;
		return 6; // DXT5
	}
	
	switch (fmt) {
		case 0x12: // OGL_RGBA_8888
		case 0x91: // VG_sRGBA_8888
		case 0x92: // VG_sRGBA_8888_PRE
		case 0x98: // VG_lRGBA_8888
		case 0x99: // VG_lRGBA_8888_PRE
		case 0x9c: // VG_sXRGB_8888
		case 0x9d: // VG_sARGB_8888
		case 0xa1: // VG_lXRGB_8888
		case 0xa2: // VG_lARGB_8888
		case 0xb4: // VG_sABGR_8888
		case 0xb9: // VG_lABGR_8888
			c0 = 'r'; c1 = 'g'; c2 = 'b'; c3 = 'a';
			d0 = 8; d1 = 8; d2 = 8; d3 = 8;
			gl_format = 0x1908; bpp = 4;
			return 10;
			
		case 0xa4: // VG_sBGRX_8888
		case 0xa5: // VG_sBGRA_8888
		case 0xab: // VG_lBGRX_8888
		case 0xac: // VG_lBGRA_8888
			c0 = 'b'; c1 = 'g'; c2 = 'r'; c3 = 'a';
			d0 = 8; d1 = 8; d2 = 8; d3 = 8;
			gl_format = 0x80E1; bpp = 4;
			return 10;
			
		case 0x93: // VG_sRGB_565
		case 0xa7: // VG_sBGR_565
		case 0x13: // OGL_RGB_565
			c0 = 'r'; c1 = 'g'; c2 = 'b'; c3 = 0;
			d0 = 5; d1 = 6; d2 = 5; d3 = 0;
			gl_format = 0x1907; gl_type = 0x8363; bpp = 2;
			return 10;
		case 0x15: // OGL_RGB_888
			c0 = 'r'; c1 = 'g'; c2 = 'b'; c3 = 0;
			d0 = 8; d1 = 8; d2 = 8; d3 = 0;
			gl_format = 0x1907; bpp = 3;
			return 10;
			
		case 0x95: // VG_sRGBA_4444
		case 0xaa: // VG_sBGRA_4444
		case 0x14: // OGL_RGBA_4444
		case 0x00: // MGL_ARGB_4444
			c0 = 'r'; c1 = 'g'; c2 = 'b'; c3 = 'a';
			d0 = 4; d1 = 4; d2 = 4; d3 = 4;
			gl_format = 0x1908; gl_type = 0x8033; bpp = 2;
			return 10;
			
		case 0x94: // VG_sRGBA_5551
		case 0xa9: // VG_sBGRA_5551
		case 0x11: // OGL_RGBA_5551
			c0 = 'r'; c1 = 'g'; c2 = 'b'; c3 = 'a';
			d0 = 5; d1 = 5; d2 = 5; d3 = 1;
			gl_format = 0x1908; gl_type = 0x8034; bpp = 2;
			return 10;
			
		case 0x96: // VG_sL_8
		case 0x9a: // VG_lL_8
		case 0x16: // OGL_I_8
		case 0x07: // MGL_I_8
			c0 = 'l'; c1 = 0; c2 = 0; c3 = 0;
			d0 = 8; d1 = 0; d2 = 0; d3 = 0;
			gl_format = 0x1909; bpp = 1;
			return 10;
			
		case 0x9b: // VG_A_8
		case 0x17: // OGL_A_8
		case 0x1b: // GL_A_8
			c0 = 'a'; c1 = 0; c2 = 0; c3 = 0;
			d0 = 8; d1 = 0; d2 = 0; d3 = 0;
			gl_format = 0x1906; bpp = 1;
			return 10;
			
		default:
			// Full backward compatibility fallback: assume ETC1
			gl_format = 0x1908; bpp = 4;
			return 1;
	}
}

} // namespace pvr

