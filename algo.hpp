#pragma once

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <omp.h>
#include <set>
#include <span>
#include <string>
#include <time.h>
#include <vector>

#include <seqan/align.h>
#include <seqan/align_parallel.h>
#include <seqan/arg_parse.h>
#include <seqan/bam_io.h>
#include <seqan/basic.h>
#include <seqan/seq_io.h>
#include <seqan/sequence.h>
#include <seqan/store.h>
#include <seqan/stream.h>
#include <seqan/vcf_io.h>

#include "misc.hpp"
#include "options.hpp"

// Sequence, alignment, and alignment row.
typedef seqan::String<seqan::Dna5>                TSequence;
typedef seqan::Align<TSequence, seqan::ArrayGaps> TAlign;
typedef seqan::Row<TAlign>::Type                  TRow;
using ssize_t   = std::make_signed_t<size_t>;
using TSeqInfix = seqan::Segment<TSequence const, seqan::InfixSegment>;

#define LL_THRESHOLD -25.5
#define LG10         3.322
#define NO_ALIGNMENT -10000

inline constexpr size_t NO_BEST = size_t(-1ull);

/* Stores information of how a read aligns across a variant */
class varAlignInfo
{
public:
    std::string qname;
    size_t              nD;
    size_t              nI;
    size_t              nAlleles;
    std::vector<double> alignS;
    bool                softClipped;
    bool                alignsLeft;
    bool                alignsRight;
    // int refAlignLen;

    // Supports and rejects logic is not correct for very long variants
    // Alternate allele is supported as judged from bam alignment record
    bool supports(float const refLen, float const altLen, LRCOptions const & O) const
    {
        if (altLen > refLen)
        { // insertion (these are simplistic for insertion/deletion type variants)
            // doesn't work properly if alt and ref are of similar size

            return ((alignsLeft && alignsRight && ((float)nI > altLen * O.altThreshFraction) &&
                     ((float)nI < altLen * O.altThreshFractionMax)) ||
                    softClipped);
        }
        else
        {
            return ((alignsLeft && alignsRight && ((float)nD > refLen * O.altThreshFraction) &&
                     ((float)nD < refLen * O.altThreshFractionMax)) ||
                    softClipped);
        }
    }

    // Alternate allele is rejected as judged from bam alignment record
    bool rejects(float const refLen, float const altLen, LRCOptions const & O) const
    {
        if (altLen > refLen)
        { // insertion
            return ((alignsLeft && alignsRight && ((float)nI < altLen * O.refThreshFraction)) && (!softClipped));
        }
        else
        {
            return ((alignsLeft && alignsRight && ((float)nD < refLen * O.refThreshFraction)) && (!softClipped));
        }
    }

    bool present(LRCOptions const & O) const
    {
        return (nI >= O.minPresent || nD >= O.minPresent);
    }

    bool aligns() const
    {
        return (alignsLeft && alignsRight);
    }

    void reset()
    {
        nD = 0;
        nI = 0;

        for (size_t i = 0; i < alignS.size(); i++)
            alignS[i] = NO_ALIGNMENT;

        softClipped = false;
        alignsLeft  = false;
        alignsRight = false;
    }

    // Likelihood of variant relative to the most likely,
    // Updates pref values. A value x, represents that the allele is 2^-x less likely than the most likely allele
    // Returns an index to the most likely allele, if one exists
    size_t alignmentPreference(size_t const wSizeActual, LRCOptions const & O, std::vector<double> & pref) const
    {
        int    maxScore      = alignS[0];
        size_t maxI          = 0;
        int    minAlignScore = static_cast<double>(wSizeActual) * 1.2;

        for (size_t i = 0; i < nAlleles; i++)
        {
            // pref[i] = 0;
            if (alignS[i] > maxScore)
            {
                maxI     = i;
                maxScore = alignS[i];
            }
        }

        if (maxScore == NO_ALIGNMENT || maxScore <= minAlignScore)
        {
            return NO_BEST;
        }
        else
        {
            for (size_t i = 0; i < nAlleles; i++)
            {
                float d = (maxScore - alignS[i]) / O.logScaleFactor;
                if (alignS[i] == NO_ALIGNMENT || alignS[i] <= minAlignScore)
                {
                    d = (maxScore - minAlignScore) / O.logScaleFactor;
                }

                if (d > O.maxAlignBits)
                    d = O.maxAlignBits;
                if (d < 0 && O.verbose)
                    std::cerr << "WTF negative alignment score";
                pref[i] += d;
            }

            return maxI;
        }
    }

    // Likelihood of variant relative to the most likely,
    // Increments pref values. A value x, represents that the allele is 2^-x less likely than the most likely allele
    // Returns an index to the most likely allele, if one exists
    size_t vaPreference(LRCOptions const &          O,
                        size_t const                refLen,
                        std::vector<size_t> const & altLens,
                        std::vector<double> &       pref) const
    {
        if (softClipped)
        { // Softclipped, doesn't support the reference, all other alleles are equally likely
            pref[0] += O.overlapBits;
            return NO_BEST;
        }

        if (!alignsLeft || !alignsRight)
        {
            return NO_BEST;
        }

        // Count number of deletions && insertions, find the variant that is closest in size

        ssize_t insDel = nI - nD;
        ssize_t minD   = std::abs(insDel);
        size_t  minDi  = 0;

        for (size_t i = 1; i < nAlleles; i++)
        {
            ssize_t cD = altLens[i - 1] - refLen;
            if (std::abs(cD - insDel) < minD)
            {
                minDi = i;
                minD  = std::abs(cD - insDel);
            }
        }

        for (size_t i = 0; i < nAlleles; i++)
        {
            if (i != minDi)
                pref[i] += O.overlapBits;
        }

        return minDi;
    }

    //  varAlignInfo(): nD(0),nI(0),
    //  refS(NO_ALIGNMENT),altS(NO_ALIGNMENT),softClipped(false),alignsLeft(false),alignsRight(false){};
    varAlignInfo(size_t const nAllelesIn)
    {
        nAlleles = nAllelesIn;
        nD       = 0;
        nI       = 0;
        alignS.resize(nAlleles);

        for (size_t i = 0; i < nAlleles; i++)
        {
            alignS[i] = NO_ALIGNMENT;
        }

        softClipped = false;
        alignsLeft  = false;
        alignsRight = false;
    }

    varAlignInfo() : varAlignInfo{2}
    {}
};

/* Turns genotyping into std::string */
void getGtString(std::vector<double> &       lls,
                 std::vector<size_t> const & ads,
                 std::vector<size_t> const & vas,
                 std::vector<std::string> & va_reads,
                 std::string &               gtString)
{
    size_t gtLen = lls.size();
    for (size_t i = 0; i < gtLen; i++)
        lls[i] = -lls[i]; // Really silly hack for historical reasons, would confuse the hell out of me to fix it

    //  int max = 0;
    double maxP  = lls[0];
    size_t a1    = 0;
    size_t a2    = 0;
    size_t maxA1 = 0;
    size_t maxA2 = 0;

    for (size_t i = 0; i < gtLen; i++)
    {
        if (lls[i] > maxP)
        {
            maxP  = lls[i];
            //      max = i;
            maxA1 = a1;
            maxA2 = a2;
        }

        if (a2 < a1)
        {
            a2++;
        }
        else
        {
            a1++;
            a2 = 0;
        }
    }
    std::ostringstream buff;
    buff << maxA2 << "/" << maxA1 << ":";
    for (size_t i = 0; i < ads.size() - 1; i++)
        buff << ads[i] << ",";
    buff << ads[ads.size() - 1] << ":";

    for (size_t i = 0; i < vas.size() - 1; i++)
        buff << vas[i] << ",";
    buff << vas[vas.size() - 1] << ":";

    for (size_t i = 0; i < gtLen; i++)
    {
        double lp = (lls[i] - maxP) / LG10;
        if (lp < LL_THRESHOLD)
            lp = LL_THRESHOLD;
        buff << int(-10 * lp);
        if (i != gtLen - 1)
            buff << ",";
    }
    buff << ":";
    buff << va_reads[0]<< ":";
    buff << va_reads[1];
    gtString = buff.str();
}

// Input: variant and seqan::VarAlignInfo records for each read overlapping variant
// Output: Relative genotype likelihoods in log_2 scale and read info counts
inline void multiUpdateVC(seqan::VcfRecord const &          var,
                          std::vector<varAlignInfo> const & vais,
                          std::vector<double> &             vC,
                          std::vector<size_t> &             rI,
                          std::vector<size_t> &             VAs,
                          std::vector<std::string> &        VA_qnames,
                          size_t const                      wSizeActual,
                          LRCOptions const &                O,
                          genotyping_model const            gtm)
{
    seqan::StringSet<seqan::CharString> altSet;
    strSplit(altSet, var.alt, seqan::EqualsChar<','>());

    size_t nAlts = length(altSet); // TODO: = var.nAlts

    std::vector<size_t> altLens;
    altLens.resize(nAlts);
    for (size_t i = 0; i < nAlts; i++)
    {
        altLens[i] = length(altSet[i]);
    }

    // Loop over all reads in bam file(s) 1 (deletion biased calls)
    for (auto & vai : vais)
    {
        std::vector<double> prefs;
        prefs.resize(nAlts + 1);
        // read does not occur in bam file(s) 2 (insertion biased calls)
        if (gtm == genotyping_model::ad || gtm == genotyping_model::joint)
        {
            size_t bestI = vai.alignmentPreference(wSizeActual, O, prefs);
            if (bestI != NO_BEST)
                rI[bestI]++;
            rI[rI.size() - 1]++;
        }

        if (gtm == genotyping_model::va || gtm == genotyping_model::joint)
        {
//            if(vai.qname == "m64011_190901_095311/143853997/ccs") {
//                int t = 9;
//            }
            size_t bestI = vai.vaPreference(O, length(var.ref), altLens, prefs);
            if (bestI != NO_BEST) {
                VAs[bestI]++;
                VA_qnames[bestI] = VA_qnames[bestI] + ","+vai.qname;
            }
            VAs[VAs.size() - 1]++;
            if (O.verbose)
                std::cerr << "va " << /*TODO vai.first <<*/ " " << vai.nD << " " << vai.nI << " " << prefs[0] << " "
                          << prefs[1] << " " << bestI << '\n';
        }

        if (gtm == genotyping_model::va_old)
        {
            size_t bestI     = 0;
            double bestScore = 0; // std::numeric_limits< float>::max();
            for (size_t iP = 0; iP < nAlts; iP++)
            {
                double cScore =
                  O.overlapBits * (-vai.supports(length(var.ref), altLens[iP], O) * 1.0 +
                                   vai.rejects(length(var.ref), altLens[iP], O) * 1.0); // ACHTUNG: needs fixing
                prefs[iP + 1] += cScore;
                if (cScore < bestScore)
                {
                    bestScore = cScore;
                    bestI     = iP + 1;
                }
            }
            VAs[bestI]++;
            VAs[VAs.size() - 1]++;
            if (O.verbose)
                std::cerr << "va_old " << /*TODO vai.first <<*/ " " << vai.nD << " " << vai.nI << " " << prefs[0] << " "
                          << prefs[1] << " " << bestI << '\n';
        }

        if (gtm == genotyping_model::presence)
        {
            if (vai.present(O))
            {
                prefs[0] += O.overlapBits;
            }
            else
            {
                prefs[1] += O.overlapBits;
            }
            for (size_t iP = 2; iP <= nAlts; iP++)
            {
                prefs[iP] += O.overlapBits;
            }
        }

        //    if( O.verbose ) std::cerr << "Update VC bam1 " << vai.first << " " << prefs[0] << " " << prefs[1] << " "
        //    <<
        //    vai.supports( (int) length( var.ref ), (int) length( altSet[0] ), O ) << " " <<
        //    vai.rejects( (int) length( var.ref), (int) length( altSet[0]), O ) << '\n';

        float minPref = std::numeric_limits<float>::max();
        float maxPref = std::numeric_limits<float>::lowest();
        for (size_t iP = 0; iP < nAlts + 1; iP++)
        {
            if (prefs[iP] < minPref)
                minPref = prefs[iP];
            if (prefs[iP] > maxPref)
                maxPref = prefs[iP];
        }
        for (size_t iP = 0; iP < nAlts + 1; iP++)
            prefs[iP] -= minPref;

#define MINIMUM_PREF_DIFF 2.0

        if (maxPref - minPref > MINIMUM_PREF_DIFF)
        {
            size_t vCI = 0;
            for (size_t a1 = 0; a1 < nAlts + 1; a1++)
            {
                for (size_t a2 = 0; a2 <= a1; a2++)
                {
                    if (a1 != a2)
                    {
                        if (prefs[a1] == prefs[a2])
                        {
                            vC[vCI] += prefs[a1];
                        }
                        else if (prefs[a1] > prefs[a2] + 2)
                        {
                            vC[vCI] += prefs[a2] + 1;
                        }
                        else if (prefs[a2] > prefs[a1] + 2)
                        {
                            vC[vCI] += prefs[a1] + 1;
                        }
                        else if (prefs[a1] > prefs[a2])
                        {
                            vC[vCI] += (prefs[a1] + prefs[a2]) / 2.0;
                        }
                    }
                    else
                    {
                        vC[vCI] += prefs[a1];
                    }
                    vCI++;
                }
            }
        }
    }

    if (O.verbose)
        std::cerr << "multiUpdateVC " << vC[0] << " " << vC[1] << " " << vC[2] << '\n';
}

// Input: seqan::VcfRecord, reference and alternate reference
// Output: Chrom and position of variant, ref sequence and alt sequence
// TODO: Change this for a library that does this
inline void getLocRefAlt(seqan::VcfRecord const &  variant,
                         seqan::FaiIndex const &   faiI,
                         seqan::CharString const & chrom,
                         TSequence &               refSeq,
                         std::vector<TSequence> &  altSeqs,
                         size_t const              wSizeActual,
                         LRCOptions const &        O)
{
    TSequence                           ref = variant.ref;
    seqan::StringSet<seqan::CharString> altSet;
    strSplit(altSet, variant.alt, seqan::EqualsChar<','>());

    size_t nAlts = length(altSet);

    int32_t  beginPos = variant.beginPos;
    unsigned idx      = 0;

    if (!getIdByName(idx, faiI, chrom))
    {
        if (O.verbose)
            std::cerr << "rID " << chrom << " " << beginPos
                      << " WARNING: reference FAI index has no entry for rID in Ref std::mapped.\n";
    }
//    beginPos = 181600000;
    if (O.genotypeRightBreakpoint)
        readRegion(refSeq, faiI, idx, beginPos - wSizeActual + length(ref), beginPos + length(ref) + wSizeActual);
    else
        readRegion(refSeq, faiI, idx, beginPos - wSizeActual, beginPos + wSizeActual);

    if (O.verbose)
        std::cerr << "refSeq " << refSeq << " " << chrom << " " << beginPos << std::endl;

    std::vector<size_t> altLens;
    altLens.resize(nAlts);
    for (size_t i = 0; i < nAlts; i++)
        altLens[i] = length(altSet[i]);

    size_t refLen = length(variant.ref);
    for (size_t i = 0; i < nAlts; i++)
    {
        if (!O.genotypeRightBreakpoint)
        {
            readRegion(altSeqs[i],
                       faiI,
                       idx,
                       beginPos - wSizeActual,
                       beginPos); // beginPos is included in the alt sequence
            TSequence post;
            if (altLens[i] < (size_t)wSizeActual)
            {
                post = altSet[i];
                TSequence post2;
                readRegion(post2, faiI, idx, beginPos + refLen, beginPos + refLen + wSizeActual - altLens[i]);
                append(post, post2);
            }
            else
            {
                post = infixWithLength(altSet[i], 0, wSizeActual);
            }
            append(altSeqs[i], post);
        }
        else
        {
            if (altLens[i] < (size_t)wSizeActual)
            {
                readRegion(altSeqs[i], faiI, idx, beginPos - wSizeActual + altLens[i], beginPos);
                append(altSeqs[i], altSet[i]);
            }
            else
            {
                altSeqs[i] = infixWithLength(altSet[i], altLens[i] - wSizeActual, wSizeActual);
            }
            TSequence post;
            readRegion(post, faiI, idx, beginPos + refLen, beginPos + refLen + wSizeActual);
            append(altSeqs[i], post);
        }
    }

    if (O.verbose)
    {
        std::cerr << "Printing altSeq " << '\n';
        for (size_t i = 0; i < nAlts; i++)
            std::cerr << "altSeq " << i << " " << altSeqs[i] << '\n';
        std::cerr << "Done printing altSeq " << '\n';
    }
}

// Crops a subsequence in a seqan::BamAlignmentRecord and rewrites into the seqan::BamAlignmentRecord
inline void cropSeq(seqan::BamAlignmentRecord const & bar,
                    seqan::VcfRecord const &          var,
                    ssize_t const                     wSizeActual, // <- this is signed here!
                    LRCOptions const &                O,
                    TSequence &                       croppedSeq)
{
    auto &  cigarString    = bar.cigar;
    ssize_t alignPos       = bar.beginPos;
    ssize_t readPos        = 0;
    ssize_t lReadPos       = 0;
    size_t  cigarI         = 0;
    char    cigarOperation = cigarString[cigarI].operation;

    // Searches for the first position overlapping our window (right insert) or last position overlapping window (left
    // insert)
    // TODO the following can underflow :o
    ssize_t searchPos = var.beginPos - wSizeActual; // Want to change the search intervals for TRs
    if (O.genotypeRightBreakpoint)
        searchPos = var.beginPos + length(var.ref) + wSizeActual;

    searchPos = std::max<ssize_t>(searchPos, 0);

    while (alignPos < searchPos && cigarI < length(cigarString))
    {
        // lAlignPos = alignPos;
        lReadPos       = readPos;
        cigarOperation = cigarString[cigarI].operation;

        switch (cigarOperation)
        {
            case 'D':
                alignPos += cigarString[cigarI].count;
                break;
            case '=':
            case 'X':
            case 'M':
                alignPos += cigarString[cigarI].count;
                [[fallthrough]];
            case 'S':
            case 'I':
                readPos += cigarString[cigarI].count;
                break;
            case 'H': // this is untested
                break;
            default:
                std::cerr << "WARNING: cigar string case not accounted for \n";
        }

        if (O.verbose)
            std::cerr << bar.qName << " readpos " << alignPos << " " << var.beginPos << " " << searchPos << " "
                      << cigarI << " " << readPos << " " << cigarString[cigarI].count << " " << cigarOperation << '\n';
        cigarI++;
    }
    if (alignPos < searchPos && O.verbose)
    {
        std::cerr << "Read clipped " << alignPos << " " << var.beginPos << " " << searchPos << " " << cigarI << " "
                  << length(cigarString) << '\n';
    }

    if (cigarOperation == 'S' || cigarOperation == 'H')
        readPos = lReadPos;

    ssize_t rBeg = 0;
    ssize_t rEnd = 0;
    if (O.genotypeRightBreakpoint)
    {
        //    if( alignPos >= searchPos ){
        //   rBeg = readPos - 2*wSizeActual;
        // rEnd = readPos;
        //}else
        if (alignPos >= searchPos - 2 * wSizeActual)
        {
            ssize_t rShift = searchPos - alignPos;
            rBeg           = readPos - 2 * wSizeActual + rShift;
            rEnd           = readPos + rShift;
        }
        else
        {
            rBeg = readPos;
            rEnd = readPos + wSizeActual;
            if (O.verbose)
                std::cerr << "Insensible case for read " << bar.qName << " " << alignPos << " " << var.beginPos << " "
                          << searchPos << " " << cigarI << " " << length(cigarString) << '\n';
        }
    }
    else
    {
        ssize_t rShift = alignPos - searchPos;
        rBeg           = readPos - rShift;
        rEnd           = readPos + 2 * wSizeActual - rShift;
        if (rShift < 0 && O.verbose)
            std::cerr << "Poorly formatted read, case not accounted for " << bar.qName << " " << alignPos << " "
                      << var.beginPos << " " << searchPos << " " << cigarI << " " << length(cigarString) << '\n';
    }
    if (O.verbose)
        std::cerr << "Cropped read " << bar.qName << " " << alignPos << " " << var.beginPos << " " << searchPos << " "
                  << cigarI << " " << length(cigarString) << " " << rBeg << " " << rEnd << '\n';
    if (rBeg < 0)
        rBeg = 0;
    if (rEnd < 2 * wSizeActual)
        rEnd = 2 * wSizeActual;
    if (rEnd > (ssize_t)length(bar.seq))
        rEnd = (ssize_t)length(bar.seq);
    if (O.verbose)
        std::cerr << "ToInfix " << rBeg << " " << rEnd << " " << length(bar.seq) << '\n';
//    a little bug here
    if (rEnd == rBeg) {
        rBeg = rBeg - 1;
    }
    croppedSeq = infixWithLength(bar.seq, rBeg, rEnd - rBeg);

    if (O.verbose)
        std::cerr << "Successful crop " << bar.qName << " " << croppedSeq << '\n';
}

template <typename TSeq>
inline TSeq mask(TSeq const & in)
{
    TSeq ret;
    seqan::appendValue(ret, in[0]);

    for (size_t i = 1; i < seqan::length(in); ++i)
        if (in[i] != in[i - 1])
            seqan::appendValue(ret, in[i]);

    return ret;
}

/** Input: bamStream, a VCF entry, reference fasta and alt fasta file if required by VCF entry
    Output: variant alignment info for each read near the VCF entry
 */
inline void LRprocessReads(seqan::VcfRecord const &                               variant,
                           seqan::CharString const &                              chrom,
                           seqan::FaiIndex const &                                faiI,
                           std::vector<seqan::BamAlignmentRecord const *> const & overlappingBars,
                           std::vector<varAlignInfo> &                            vais,
                           size_t const                                           wSizeActual,
                           LRCOptions const &                                     O)
{
    if (variant.beginPos == 189408303) {
        int tmp = 0;
    }
    TSequence              refSeq;
    std::vector<TSequence> altSeqs;

    seqan::StringSet<seqan::CharString> altSet;
    strSplit(altSet, variant.alt, seqan::EqualsChar<','>());

    size_t const nAlts = length(altSet);
    altSeqs.resize(nAlts);
    if (O.verbose)
        std::cerr << "nAlts " << nAlts << '\n';

    // move outside of this function
    getLocRefAlt(variant, faiI, chrom, refSeq, altSeqs, wSizeActual, O);

    if (O.outputRefAlt)
    {
        std::cerr << chrom << " " << /*beginPos +*/ 1 << " " << variant.info << " " << refSeq;
        for (size_t i = 0; i < nAlts; i++)
            std::cerr << " " << altSeqs[i];
        std::cerr << '\n';
        return;
    }

    if (O.mask)
        refSeq = mask(refSeq);

    seqan::Score<int32_t, seqan::Simple> scoringScheme32(O.match, O.mismatch, O.gapExtend, O.gapOpen);
    seqan::Score<int16_t, seqan::Simple> scoringScheme16(O.match, O.mismatch, O.gapExtend, O.gapOpen);

    double const band_fac = std::min<double>(O.bandedAlignmentPercent, 100.0) / 100.0;

    // The set of alleles is the same for all alignments
    std::vector<TSeqInfix> seqsV;
    seqsV.resize(nAlts + 1);
    seqsV[0] = infix(refSeq, 0, seqan::length(refSeq));
    for (size_t j = 0; j < nAlts; ++j)
        seqsV[j + 1] = infix(altSeqs[j], 0, seqan::length(altSeqs[j]));

    int32_t const vBand = static_cast<double>(seqan::length(refSeq)) * band_fac;

    // This is a set that contains the respective read at every position [needs to be same size as other set]
    std::vector<TSeqInfix> seqsH;
    seqsH.resize(nAlts + 1);
    TSequence seqToAlign;

    seqan::ExecutionPolicy<seqan::Serial, seqan::Vectorial> execP;

    for (size_t i = 0; i < overlappingBars.size(); ++i)
    {
        seqan::BamAlignmentRecord const & b   = *overlappingBars[i];
        varAlignInfo &                    vai = vais[i];
        if (b.qName == "m64012_190921_234837/122881456/ccs") {
            int tmp = 0;
        }
        seqan::clear(seqToAlign);
        int tmp3 = (ssize_t)length(b.seq);

        if (O.cropRead)
            cropSeq(b, variant, wSizeActual, O, seqToAlign);
        else
            seqToAlign = b.seq; // converts IUPAC to Dna5
        for (auto & seqH : seqsH)
            seqH = infix(seqToAlign, 0, seqan::length(seqToAlign));

        int32_t const hBand = static_cast<double>(seqan::length(seqToAlign)) * band_fac;

        if (seqan::length(refSeq) > std::numeric_limits<int16_t>::max() &&
            seqan::length(seqToAlign) > std::numeric_limits<int16_t>::max())
        {
            auto scores = seqan::localAlignmentScore(execP, seqsH, seqsV, scoringScheme32, -vBand, +hBand);

            for (size_t j = 0; j < seqan::length(vai.alignS); ++j)
                vai.alignS[j] = scores[j];
        }
        else // this allows better vectorisation
        {
            auto scores = seqan::localAlignmentScore(execP, seqsH, seqsV, scoringScheme16, -vBand, +hBand);

            for (size_t j = 0; j < seqan::length(vai.alignS); ++j)
                vai.alignS[j] = scores[j];
        }
    }
}

inline void initializeBam(std::string fileName, seqan::BamIndex<seqan::Bai> & bamIndex, seqan::BamFileIn & bamStream)
{
    if (!seqan::open(bamStream, fileName.data()))
        throw error{"Could not open ", fileName, " for reading."};

    fileName += ".bai";
    if (!seqan::open(bamIndex, fileName.data()))
        throw error{"Could not read BAI index file ", fileName};

    seqan::BamHeader header;
    readHeader(header, bamStream);
}

// Open a bam file or a set of bam files if the filename does not end with .bam
inline void parseBamFileName(std::filesystem::path const &              bfN,
                             std::vector<seqan::BamFileIn> &            bamStreamV,
                             std::vector<seqan::BamIndex<seqan::Bai>> & bamIndexV,
                             LRCOptions const &                         O)
{
    std::vector<std::filesystem::path> paths;

    if (bfN.native().ends_with(".bam") || bfN.native().ends_with(".sam.gz"))
    {
        paths.push_back(bfN);
    }
    else
    {
        std::ifstream fS;
        fS.open(bfN);
        std::string bf;
        while (fS >> bf)
            paths.push_back(bf);
    }

    if (O.verbose)
        std::cerr << "Checking input files" << (O.cacheDataInTmp ? " and copying to cache dir..." : "...");

    for (std::filesystem::path & p : paths)
    {
        if (!p.native().ends_with(".bam") && !p.native().ends_with(".sam.gz"))
            throw error{"Input file '", p, "' has unrecognized extension."};

        if (!std::filesystem::exists(p))
            throw error{"Input file '", p, "' does not exist."};

        std::filesystem::path p_bai = p;
        p_bai += ".bai";
        if (!std::filesystem::exists(p_bai))
            throw error{"Input file '", p, "' has no corresponding '.bai' index."};

        if (O.cacheDataInTmp) // copy to tmp
        {
            std::filesystem::path new_p     = O.cacheDir / p.filename();
            std::filesystem::path new_p_bai = O.cacheDir / p_bai.filename();

            if (std::filesystem::exists(new_p) || std::filesystem::exists(new_p_bai))
                throw error{"Cache file already exists. Does a filename appear twice in input?"};

            std::filesystem::copy(p, new_p);
            std::filesystem::copy(p_bai, new_p_bai);

            p = new_p; // update path in-place
        }
    }

    if (O.verbose)
        std::cerr << " done.";

    bamIndexV.resize(paths.size());
    bamStreamV.resize(paths.size());

    for (size_t i = 0; i < paths.size(); ++i)
        initializeBam(paths[i], bamIndexV[i], bamStreamV[i]);
}

// Examines a seqan::BamAlignmentRecord for evidence of supporting a variant and writes evidence into varAlignInfo
// record
inline void examineBamAlignment(seqan::BamAlignmentRecord const & bar,
                                seqan::VcfRecord const &          var,
                                varAlignInfo &                    vai,
                                LRCOptions const &                O)
{
    vai.reset();

    auto &  cigarString    = bar.cigar;
    int32_t alignPos       = bar.beginPos;
    size_t  cigarI         = 0;
    char    cigarOperation = cigarString[cigarI].operation;
    int32_t regionBeg      = var.beginPos - O.varWindow;
    int32_t regionEnd      = var.beginPos + (int32_t)length(var.ref) + O.varWindow;
    vai.qname = seqan::toCString(bar.qName);
    seqan::StringSet<seqan::CharString> infos;
    strSplit(infos, var.info, seqan::EqualsChar<';'>());

    for (size_t i = 0; i < length(infos); i++)
    {
        seqan::StringSet<seqan::CharString> info2;
        strSplit(info2, infos[i], seqan::EqualsChar<'='>());
        int32_t cVal;
        if (info2[0] == "TRRBEGIN")
        {
            if (info2[1] != ".")
            {
                cVal = std::atoi(toCString(info2[1])) - O.varWindow;
                if (cVal < regionBeg)
                    regionBeg = cVal;
            }
        }
        if (info2[0] == "TRREND")
        {
            if (info2[1] != ".")
            {
                cVal = std::atoi(toCString(info2[1])) + O.varWindow;
                if (cVal > regionEnd)
                    regionEnd = cVal;
            }
        }
        if (info2[0] == "REGBEGIN")
        {
            if (info2[1] != ".")
            {
                cVal = std::atoi(toCString(info2[1])) - O.varWindow;
                if (cVal < regionBeg)
                    regionBeg = cVal;
            }
        }
        if (info2[0] == "REGEND")
        {
            if (info2[1] != ".")
            {
                cVal = std::atoi(toCString(info2[1])) + O.varWindow;
                if (cVal > regionEnd)
                    regionEnd = cVal;
            }
        }
    }
    /*  if( infos.count( "TRRBEGIN" ) == 1 and infos.count( "TRREND" ) == 1 ){
      regionBeg = std::atoi(var.infos["TRRBEGIN"] ) - O.varWindow;
      seqan::CharString endStr = getValueById( var.genotypeInfos, trrEndID);
      regionEnd = std::atoi( var.infos["TRREND"] ) + O.varWindow;
      if( O.verbose ) std::cerr << "Found TRR " << regionBeg << " " << regionEnd << '\n';
  }else{*/
    if (O.verbose)
        std::cerr << "TRR " << regionBeg << " " << regionEnd << '\n';
    //  }

    if (alignPos  < regionBeg)
        vai.alignsLeft = true;

    // Find the first position that overlaps the window we are interested in
    while (alignPos < regionBeg && cigarI < length(cigarString))
    {
        cigarOperation = cigarString[cigarI].operation;
        if (cigarOperation == 'M' || cigarOperation == '=' || cigarOperation == 'D' || cigarOperation == 'X')
        {
            alignPos += cigarString[cigarI].count;
        }
        cigarI++;
    }

    // only counts the number of deleted bp in the window, probably fine to count a longer distance
    if (alignPos > regionBeg && cigarOperation == 'D' && (alignPos - (regionBeg) >= (int)O.minDelIns))
    {
        vai.nD = alignPos - regionBeg;
    }

    while (alignPos < regionEnd && cigarI < length(cigarString))
    {
        cigarOperation = cigarString[cigarI].operation;
        switch (cigarOperation)
        {
            case 'D':
                if (cigarString[cigarI].count >= O.minDelIns)
                    vai.nD += cigarString[cigarI].count;
                [[fallthrough]];
            case '=':
            case 'X':
            case 'M':
                alignPos += cigarString[cigarI].count;
                break;
            case 'I':
                if (cigarString[cigarI].count >= O.minDelIns)
                    vai.nI += cigarString[cigarI].count;
                break;
            case 'S':
                if (cigarString[cigarI].count > O.maxSoftClipped)
                {
                    if (!O.genotypeRightBreakpoint)
                    {
                        if (cigarI == length(cigarString) - 1)
                            vai.softClipped = true;
                    }
                    else
                    {
                        if (cigarI == 0)
                            vai.softClipped = true;
                    }
                }
                break;
            case 'H': // this is untested
                break;
            default:
                std::cerr << "WARNING: cigar string case not accounted for \n";
        }

        if (O.verbose)
            std::cerr << bar.qName << " " << cigarOperation << " " << cigarString[cigarI].count << " " << alignPos
                      << " " << cigarI << " " << vai.nD << " " << vai.nI << '\n';
        cigarI++;
    }
    if (alignPos > regionEnd)
        vai.alignsRight = true;

    if (O.verbose)
        std::cerr << "examinSeq " << bar.qName << " " << vai.nD << " " << vai.nI << " " << vai.softClipped << '\n';
}

// Gets reads in the region overlapping the variant
inline void parseReads(std::vector<seqan::BamAlignmentRecord> const &   bars,
                       seqan::VcfRecord const &                         var,
                       std::vector<seqan::BamAlignmentRecord const *> & overlappingBars,
                       std::vector<varAlignInfo> &                      align_infos,
                       size_t const                                     wSizeActual,
                       LRCOptions const &                               O)
{
    int32_t beg = var.beginPos - wSizeActual;
    int32_t end = var.beginPos + wSizeActual;

    if (O.genotypeRightBreakpoint)
    {
        beg = beg + length(var.ref);
        end = end + length(var.ref);
    }

    seqan::StringSet<seqan::CharString> altSet;
    strSplit(altSet, var.alt, seqan::EqualsChar<','>());

    size_t       nAlts = length(altSet);
    varAlignInfo vai(nAlts + 1);

    int32_t stopReading = beg;
    if (O.genotypeRightBreakpoint)
        stopReading = end;

    // TODO evaluate unordered_map here
    std::map<std::string_view, size_t> nameCache;

    for (seqan::BamAlignmentRecord const & record : bars)
    {
        if (overlappingBars.size() >= O.maxBARcount || record.beginPos > stopReading) // || record.rID != rID)
            return;

        // Ignore the read if it does not stretch to the region we are interested in
        if ((record.beginPos + (int32_t)length(record.seq) < beg) ||
            (record.beginPos + (int32_t)getAlignmentLengthInRef(record) < beg) || record.mapQ < O.minMapQ)
            continue;

        examineBamAlignment(record, var, vai, O);

        if (O.verbose)
            std::cerr << "Read record " << record.qName << '\n';

        // If we are on the next reference or at the end already then we stop.
        if (/*record.rID == -1 || record.rID > rID || */ record.beginPos >= end)
            break;

        // If we are left of the selected position then we skip this record.
        bool softClipRemove = false;
        if (not O.genotypeRightBreakpoint)
        {
            char cigarOperation = record.cigar[0].operation;
            if (cigarOperation == 'S' && (record.cigar[0].count > O.maxSoftClipped))
            {
                softClipRemove = true;
                if (O.verbose)
                    std::cerr << "SoftClip removed LeftBreakpoint " << record.qName << " " << O.genotypeRightBreakpoint
                              << " " << record.cigar[0].count << " " << O.maxSoftClipped << '\n';
            }
        }
        else if (O.genotypeRightBreakpoint)
        {
            char cigarOperation = record.cigar[length(record.cigar) - 1].operation;
            if (cigarOperation == 'S' && (record.cigar[length(record.cigar) - 1].count > O.maxSoftClipped))
            {
                softClipRemove = true;
                if (O.verbose)
                    std::cerr << "SoftClip removed RightBreakpoint " << record.qName << " " << O.genotypeRightBreakpoint
                              << " " << record.cigar[length(record.cigar) - 1].count << " " << O.maxSoftClipped << '\n';
            }
        }

        bool hardClipped     = false;
        char cigarOperationL = record.cigar[0].operation;
        char cigarOperationR = record.cigar[length(record.cigar) - 1].operation;

        if ((cigarOperationL == 'H') || (cigarOperationR == 'H'))
        {
            hardClipped = true;
            if (O.verbose)
                std::cerr << "Read " << record.qName << " is hardclipped at " << record.beginPos << '\n';
        }

        if ((!softClipRemove) && (!hasFlagDuplicate(record)) && (!hasFlagQCNoPass(record)) && (!hardClipped))
        {
            // prevent multiple alignments of the same read from being used
            std::string_view id = seqan::toCString(record.qName);
            if (nameCache.contains(id)) // replace existing
            {
                // TODO possibly check here which record is primary record and use that
                size_t index           = nameCache[id];
                overlappingBars[index] = &record;
                align_infos[index]     = vai;
            }
            else
            {
                nameCache[id] = overlappingBars.size();
                overlappingBars.push_back(&record);
                align_infos.push_back(vai); // this needs to be copied and not moved, because it is reused
            }
        }

        if (O.verbose)
            std::cerr << "Finished soft clipping " << '\n';
    }

    if (O.verbose)
        std::cerr << "Exiting readBamRegion " << '\n';
}

std::vector<std::string> split (std::string s, std::string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find (delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

inline size_t getWSizeActual(std::span<seqan::VcfRecord> vcfRecords, LRCOptions const & O)
{
    if (O.dynamicWSize)
    {
        size_t maxAlleleLength = 0;
        for (seqan::VcfRecord & var : vcfRecords)
        {
            auto res = std::string(seqan::toCString(var.info));
            auto infos = split(res,";");
            size_t svlen = 0;
            for (auto& item : infos) {
                if (item.find("SVLEN") != std::string::npos) {
                    svlen = abs(std::stoi(item.substr(6, item.length() - 1)));
                    break;
                }
            }
//            int index = res.find("SVLEN");
//            auto svlen = res.substr(index, index)
            for (size_t i = 0, len = 1; i <= seqan::length(var.alt); ++i, ++len)
            {
                if (i == seqan::length(var.alt) || var.alt[i] == ',')
                {
                    maxAlleleLength = std::max(maxAlleleLength, len - 1);
                    len             = 0;
                }
            }
            maxAlleleLength = std::max(maxAlleleLength, svlen);
        }

        return maxAlleleLength + O.wSize;
    }
    else
    {
        return O.wSize;
    }
}

inline void processChunk(std::vector<seqan::BamFileIn> &            bamFiles,
                         std::vector<seqan::BamIndex<seqan::Bai>> & bamIndexes,
                         seqan::FaiIndex &                          faIndex,
                         seqan::CharString const &                  chrom,
                         std::vector<seqan::BamAlignmentRecord> &   bars,
                         std::span<seqan::VcfRecord>                vcfRecords,
                         LRCOptions const &                         O)
{
    size_t const wSizeActual = getWSizeActual(vcfRecords, O);

    /* Determine chromosome interval to fetch records for */
    size_t genome_begin = vcfRecords.front().beginPos;
    size_t genome_end   = vcfRecords.back().beginPos + 1;

    if (O.genotypeRightBreakpoint)
    {
        size_t minVarRef = std::numeric_limits<size_t>::max();
        size_t maxVarRef = std::numeric_limits<size_t>::min();

        for (seqan::VcfRecord & var : vcfRecords)
        {
            minVarRef = std::min<size_t>(minVarRef, seqan::length(var.ref));
            maxVarRef = std::max<size_t>(maxVarRef, seqan::length(var.ref));
        }

        genome_begin += minVarRef;
        genome_end += maxVarRef;
    }

    genome_begin = wSizeActual >= genome_begin ? 1 : genome_begin - wSizeActual;
    genome_end += wSizeActual;

    /* read BAM files for this chunk */
    for (size_t i = 0; i < bamFiles.size(); ++i)
    {
        size_t bamRID = 0;
        if (seqan::getIdByName(bamRID, seqan::contigNamesCache(seqan::context(bamFiles[i])), chrom))
            viewRecords(bars, bamFiles[i], bamIndexes[i], bamRID, genome_begin, genome_end);

        // else: BAM files that have no reads spanning the desired chromosome are quietly ignored
    }

    if (bamFiles.size() > 1)
    {
        std::ranges::sort(bars,
                          [](seqan::BamAlignmentRecord const & lhs, seqan::BamAlignmentRecord const & rhs)
                          { return lhs.beginPos < rhs.beginPos; });
    }

    /* process variants */
    for (seqan::VcfRecord & var : vcfRecords)
    {
        size_t nAlleles = 2;
        for (char c : var.alt)
            if (c == ',')
                ++nAlleles;

        // Implemented as a std::vector as we may have more than one output per marker
        std::vector<std::vector<double>> vC; // variant calls
        std::vector<std::vector<size_t>> AD; // Allele depth counts
        std::vector<std::vector<size_t>> VA; // Variant seqan::Alignment counts
        std::vector<std::string> VA_QNAMES;

        if (O.gtModel == genotyping_model::multi)
        {
            vC.resize(5);
            AD.resize(5);
            VA.resize(5);
        }
        else
        {
            vC.resize(1);
            AD.resize(1);
            VA.resize(1);
        }

        for (size_t mI = 0; mI < vC.size(); mI++)
        {
            vC[mI].resize(nAlleles * (nAlleles + 1) / 2);
            AD[mI].resize(nAlleles + 1);
            VA[mI].resize(nAlleles + 1);
        }
        VA_QNAMES.resize(nAlleles + 1);

        std::vector<seqan::BamAlignmentRecord const *> overlappingBars;
        std::vector<varAlignInfo>                      alignInfos;
        parseReads(bars, var, overlappingBars, alignInfos, wSizeActual, O);

        LRprocessReads(var, chrom, faIndex, overlappingBars, alignInfos, wSizeActual, O);

        if (O.gtModel == genotyping_model::multi)
        {
            multiUpdateVC(var, alignInfos, vC[0], AD[0], VA[0],VA_QNAMES, wSizeActual, O, genotyping_model::ad);
            multiUpdateVC(var, alignInfos, vC[1], AD[1], VA[1],VA_QNAMES, wSizeActual, O, genotyping_model::va);
            multiUpdateVC(var, alignInfos, vC[2], AD[2], VA[2],VA_QNAMES, wSizeActual, O, genotyping_model::joint);
            multiUpdateVC(var, alignInfos, vC[3], AD[3], VA[3],VA_QNAMES, wSizeActual, O, genotyping_model::presence);
            multiUpdateVC(var, alignInfos, vC[4], AD[4], VA[4],VA_QNAMES, wSizeActual, O, genotyping_model::va_old);
        }
        else
        {
            multiUpdateVC(var, alignInfos, vC[0], AD[0], VA[0],VA_QNAMES, wSizeActual, O, O.gtModel);
        }

        std::string gtString;
        for (size_t mI = 0; mI < vC.size(); mI++)
        {
            getGtString(vC[mI], AD[mI], VA[mI],VA_QNAMES, gtString);
//            appendValue(var.genotypeInfos, gtString);
            var.genotypeInfos[0] = gtString;
            var.format = "GT:AD:VA:PL:REFREADS:ALTREADS";
        }
    }
}
