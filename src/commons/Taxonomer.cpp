#include "Taxonomer.h"
#include "BitManipulateMacros.h"
#include "Match.h"
#include "NcbiTaxonomy.h"
#include "printBinary.h"
#include <cstdint>
#include <sys/types.h>
#include <unordered_map>


Taxonomer::Taxonomer(const LocalParameters &par, NcbiTaxonomy *taxonomy) : taxonomy(taxonomy) {
    // Parameters
    auto mask = new uint32_t[par.spaceMask.length()];
    for(size_t i = 0, j = 0; i < par.spaceMask.length(); i++){
        mask[i] = par.spaceMask[i] - 48;
        spaceNum += (mask[i] == 0);
        if(mask[i] == 1){
            unmaskedPos[j] = (int) i;
            j++;
        }
    }
    delete[] mask;
    maxGap = par.maxGap;
    minCoveredPos = par.minCoveredPos;
    accessionLevel = par.accessionLevel;
    minSSMatch = par.minSSMatch;
    minConsCnt = par.minConsCnt;
    minConsCntEuk = par.minConsCntEuk;
    eukaryotaTaxId = par.eukaryotaTaxId;

    if (par.seqMode == 1 || par.seqMode == 2) {
        denominator = 100;
    } else {
        denominator = 1000;
    }
}

Taxonomer::~Taxonomer() {

}

void Taxonomer::assignTaxonomy(const Match *matchList,
                               size_t numOfMatches,
                               std::vector<Query> &queryList,
                               const LocalParameters &par) {
    time_t beforeAnalyze = time(nullptr);
    cout << "Analyzing matches ..." << endl;

    // Divide matches into blocks for multi threading
    size_t seqNum = queryList.size();
    MatchBlock *matchBlocks = new MatchBlock[seqNum];
    size_t matchIdx = 0;
    size_t blockIdx = 0;
    uint32_t currentQuery;
    while (matchIdx < numOfMatches) {
        currentQuery = matchList[matchIdx].qInfo.sequenceID;
        matchBlocks[blockIdx].id = currentQuery;
        matchBlocks[blockIdx].start = matchIdx;
        while ((currentQuery == matchList[matchIdx].qInfo.sequenceID) && (matchIdx < numOfMatches)) ++matchIdx;
        matchBlocks[blockIdx].end = matchIdx - 1;
        blockIdx++;
    }

    // Process each block
#pragma omp parallel default(none), shared(cout, matchBlocks, matchList, seqNum, queryList, blockIdx, par)
    {
#pragma omp for schedule(dynamic, 1)
        for (size_t i = 0; i < blockIdx; ++i) {
            chooseBestTaxon(matchBlocks[i].id,
                            matchBlocks[i].start,
                            matchBlocks[i].end,
                            matchList,
                            queryList,
                            par);
        }
    }

    for (size_t i = 0; i < seqNum; i++) {
        ++taxCounts[queryList[i].classification];
    }
    delete[] matchBlocks;
    cout << "Time spent for analyzing: " << double(time(nullptr) - beforeAnalyze) << endl;

}

void Taxonomer::chooseBestTaxon(uint32_t currentQuery,
                                size_t offset,
                                size_t end,
                                const Match *matchList,
                                vector<Query> & queryList,
                                const LocalParameters &par) {

//    if (true) {
//        cout << "# " << currentQuery << " " << queryList[currentQuery].name << endl;
//        for (size_t i = offset; i < end + 1; i++) {
//            cout << matchList[i].targetId << " " << matchList[i].qInfo.frame << " " << matchList[i].qInfo.pos << " " << int(matchList[i].hamming) <<  " "  << int(matchList[i].redundancy) << endl;
//        }
//    }
    // Get the best species for current query
    vector<const Match*> speciesMatches;
    speciesMatches.reserve(end - offset + 1);
    TaxonScore speciesScore(0, 0, 0, 0, 0);
    if (par.seqMode == 2) {
        speciesScore = getBestSpeciesMatches(speciesMatches, matchList, end, offset,
                                             queryList[currentQuery].queryLength,
                                             queryList[currentQuery].queryLength2);
    } else {
        speciesScore = getBestSpeciesMatches(speciesMatches, matchList, end, offset,
                                             queryList[currentQuery].queryLength);
    }

//    if (true) {
//        cout << "# " << currentQuery << " " << queryList[currentQuery].name << " filtered\n";
//        for (size_t i = 0; i < genusMatches.size(); i++) {
//           cout << genusMatches[i].targetId << " " << genusMatches[i].qInfo.frame << " " << genusMatches[i].qInfo.pos << " " << int(genusMatches[i].hamming) <<  " "  << int(genusMatches[i].redundancy) << endl;
//         }
//        cout << "Genus score: " << genusScore.score << "\n";
//    }

    // If there is no proper species for current query, it is un-classified.
    if (speciesScore.score == 0 || speciesScore.coverage < par.minCoverage || speciesScore.score < par.minScore) {
        queryList[currentQuery].isClassified = false;
        queryList[currentQuery].classification = 0;
        queryList[currentQuery].score = speciesScore.score;
        queryList[currentQuery].coverage = speciesScore.coverage;
        queryList[currentQuery].hammingDist = speciesScore.hammingDist;
        queryList[currentQuery].newSpecies = false;
        return;
    }

    // If there are two or more good species level candidates, find the LCA.
    if (speciesScore.LCA) {
        queryList[currentQuery].isClassified = true;
        queryList[currentQuery].classification = speciesScore.taxId;
        queryList[currentQuery].score = speciesScore.score;
        queryList[currentQuery].coverage = speciesScore.coverage;
        queryList[currentQuery].hammingDist = speciesScore.hammingDist;
        return;
    }

    // Filter redundant matches  
    // vector<const Match *> filteredMatches;
    // cout << "# " << currentQuery << " " << queryList[currentQuery].name << " filtered" << endl;
    filterRedundantMatches(speciesMatches, queryList[currentQuery].taxCnt);
    
    // If score is not enough, classify to the parent of the selected species
    if (speciesScore.score < par.minSpScore) {
        queryList[currentQuery].isClassified = true;
        queryList[currentQuery].classification = taxonomy->taxonNode(
                taxonomy->getTaxIdAtRank(speciesScore.taxId, "species"))->parentTaxId;
        queryList[currentQuery].score = speciesScore.score;
        queryList[currentQuery].coverage = speciesScore.coverage;
        queryList[currentQuery].hammingDist = speciesScore.hammingDist;
        return;
    }

    // Lower rank classification
    TaxID result = lowerRankClassification(queryList[currentQuery].taxCnt,
                                         speciesScore.taxId,
                                          queryList[currentQuery].queryLength + queryList[currentQuery].queryLength2);

    // Store classification results
    queryList[currentQuery].isClassified = true;
    queryList[currentQuery].classification = result;
    queryList[currentQuery].score = speciesScore.score;
    queryList[currentQuery].coverage = speciesScore.coverage;
    queryList[currentQuery].hammingDist = speciesScore.hammingDist;
    queryList[currentQuery].newSpecies = false;
//    if (par.printLog) {
//        cout << "# " << currentQuery << endl;
//        for (size_t i = 0; i < genusMatches.size(); i++) {
//            cout << i << " " << genusMatches[i].qInfo.pos << " " <<
//            genusMatches[i].targetId << " " << int(genusMatches[i].hamming) << endl;
//        }
//        cout << "Score: " << speciesScore.score << "  " << selectedSpecies << " "
//             << taxonomy->getString(taxonomy->taxonNode(selectedSpecies)->rankIdx)
//
//             << endl;
//    }
}

void Taxonomer::filterRedundantMatches(vector<const Match *> & speciesMatches,
                                       map<TaxID, int > & taxCnt) {
    // Sort matches by the coordinate on the query
    sort(speciesMatches.begin(), speciesMatches.end(),
         [](const Match * a, const Match * b) { return a->qInfo.pos < b->qInfo.pos; });
    
    // Remove redundant matches
    size_t matchNum = speciesMatches.size();
    for (size_t i = 0; i < matchNum;) {
        size_t currQuotient = speciesMatches[i]->qInfo.pos / 3;
        uint8_t minHamming = speciesMatches[i]->hamming;
        const Match * minHammingMatch = speciesMatches[i];
        TaxID minHammingTaxId = minHammingMatch->targetId;
        while ((i < matchNum) && (currQuotient == speciesMatches[i]->qInfo.pos / 3)) {
            if (speciesMatches[i]->hamming < minHamming) {
                minHamming = speciesMatches[i]->hamming;
                minHammingMatch = speciesMatches[i];
                minHammingTaxId = minHammingMatch->targetId;
            } else if (speciesMatches[i]->hamming == minHamming) {
                minHammingTaxId = taxonomy->LCA(minHammingTaxId, speciesMatches[i]->targetId);
            }
            i++;
        }
        taxCnt[minHammingTaxId]++;
    }
}

TaxID Taxonomer::lowerRankClassification(const map<TaxID, int> & taxCnt, TaxID spTaxId, int queryLength) {
    unsigned int maxCnt = (queryLength - 1)/denominator + 1;
    unordered_map<TaxID, TaxonCounts> cladeCnt;
    getSpeciesCladeCounts(taxCnt, cladeCnt, spTaxId);
    if (accessionLevel == 2) { // Don't do accession-level classification
        // Remove leaf nodes
        for (auto it = cladeCnt.begin(); it != cladeCnt.end(); it++) {
            TaxonNode const * taxon = taxonomy->taxonNode(it->first);
            if (strcmp(taxonomy->getString(taxon->rankIdx), "") == 0) {
                // Remove current node from its parent's children list
                cladeCnt[taxon->parentTaxId].children.erase(find(cladeCnt[taxon->parentTaxId].children.begin(),
                                                                 cladeCnt[taxon->parentTaxId].children.end(),
                                                                 it->first));
            } 
        }
        return BFS(cladeCnt, spTaxId, maxCnt);
    } else {
        return BFS(cladeCnt, spTaxId, maxCnt);
    }
}

void Taxonomer::getSpeciesCladeCounts(const map<TaxID, int> &taxCnt,
                                       unordered_map<TaxID, TaxonCounts> & cladeCount,
                                       TaxID speciesTaxID) {
    for (auto it = taxCnt.begin(); it != taxCnt.end(); ++it) {
        TaxonNode const * taxon = taxonomy->taxonNode(it->first);
        cladeCount[taxon->taxId].taxCount = it->second;
        cladeCount[taxon->taxId].cladeCount += it->second;
        while (taxon->taxId != speciesTaxID) {
            if (find(cladeCount[taxon->parentTaxId].children.begin(),
                     cladeCount[taxon->parentTaxId].children.end(),
                     taxon->taxId) == cladeCount[taxon->parentTaxId].children.end()) {
                cladeCount[taxon->parentTaxId].children.push_back(taxon->taxId);
            }
            cladeCount[taxon->parentTaxId].cladeCount += it->second;
            taxon = taxonomy->taxonNode(taxon->parentTaxId);
        }
    }
}

TaxID Taxonomer::BFS(const unordered_map<TaxID, TaxonCounts> & cladeCnt, TaxID root, unsigned int maxCnt) {
    unsigned int maxCnt2 = maxCnt;
    if (cladeCnt.at(root).children.empty()) { // root is a leaf
        return root;
    }
    unsigned int currentCnt;
    vector<TaxID> bestChildren;
    for (auto it = cladeCnt.at(root).children.begin(); it != cladeCnt.at(root).children.end(); it++) {
        currentCnt = cladeCnt.at(*it).cladeCount;
        if (currentCnt > maxCnt) {
            bestChildren.clear();
            bestChildren.push_back(*it);
            maxCnt = currentCnt;
        } else if (currentCnt == maxCnt) {
            bestChildren.push_back(*it);
        }
    }
    if (bestChildren.size() == 1) {
        return BFS(cladeCnt, bestChildren[0], maxCnt2);
    } else {
        return root;
    }
}

TaxonScore Taxonomer::getBestSpeciesMatches(vector<const Match *> & speciesMatches,
                                            const Match *matchList,
                                            size_t end,
                                            size_t offset,
                                            int queryLength) {
    vector<vector<const Match *>> matchesForEachSpecies;
    TaxonScore bestScore;
    vector<const Match *> curFrameMatches;
    vector<MatchPath> matchPaths;
    unordered_map<TaxID, float> species2score;
    unordered_map<TaxID, vector<MatchPath>> species2matchPaths;
    float bestSpScore = 0;
    unordered_map<TaxID, pair<size_t, size_t>> speciesMatchRange;

    size_t i = offset;
    while (i  < end + 1) {
        TaxID currentSpecies = matchList[i].speciesId;
        size_t start = i;
        // For current species
        while ((i < end + 1) && currentSpecies == matchList[i].speciesId) {
            uint8_t curFrame = matchList[i].qInfo.frame;
            curFrameMatches.clear();
            // For current frame
            while ((i < end + 1) && currentSpecies == matchList[i].speciesId && curFrame == matchList[i].qInfo.frame) {
                curFrameMatches.push_back(&matchList[i]);
                i ++;
            }
            if (curFrameMatches.size() > 1) {
                remainConsecutiveMatches(curFrameMatches, matchPaths, currentSpecies);
            }
        }
        speciesMatchRange[currentSpecies] = make_pair(start, i);
        // Combine MatchPaths
        if (!matchPaths.empty()) {
            // cout << currentSpecies << endl;
            float score = combineMatchPaths(matchPaths, species2matchPaths[currentSpecies], queryLength);
            // cout << endl;
            if (score > 1.0f) {score = 1.0f;}
            species2score[currentSpecies] = score;
            if (score > bestSpScore) {
                bestSpScore = score;
            }
        }
        matchPaths.clear();
    }
    
    // If there are no meaningful species
    if (species2score.empty()) {
        bestScore.score = 0;
        return bestScore;
    }

    vector<TaxID> maxSpecies;
    for (auto & spScore : species2score) {
        if (spScore.second > bestSpScore * 0.95) {
            maxSpecies.push_back(spScore.first);
        }
    }

    // More than one species --> LCA
    float coveredLength = 0.f;
    if (maxSpecies.size() > 1) {
        bestScore.LCA = true;
        bestScore.taxId = taxonomy->LCA(maxSpecies)->taxId;
        for (auto & sp : maxSpecies) {
            bestScore.score += species2score[sp];
            coveredLength = 0;
            for (auto & matchPath : species2matchPaths[maxSpecies[0]]) {
                coveredLength += matchPath.end - matchPath.start + 1;
            }
            bestScore.coverage += coveredLength / queryLength;
        }
        bestScore.score /= maxSpecies.size();
        bestScore.coverage /= maxSpecies.size();
        return bestScore;
    }
    

    // One species
    bestScore.taxId = maxSpecies[0];
    bestScore.score = species2score[maxSpecies[0]];
    int hammingDist = 0;
    for (auto & matchPath : species2matchPaths[maxSpecies[0]]) {
        coveredLength += matchPath.end - matchPath.start + 1;
        hammingDist += matchPath.hammingDist;
    }
    speciesMatches.reserve(speciesMatchRange[bestScore.taxId].second
                        - speciesMatchRange[bestScore.taxId].first + 1);

    for (size_t j = speciesMatchRange[bestScore.taxId].first; j < speciesMatchRange[bestScore.taxId].second; j++) {
        speciesMatches.push_back(& matchList[j]);
    }
    bestScore.coverage = coveredLength / queryLength;
    bestScore.hammingDist = hammingDist;
    
    return bestScore;                                  
}

TaxonScore Taxonomer::getBestSpeciesMatches(vector<const Match *> & speciesMatches,
                                            const Match *matchList,
                                            size_t end,
                                            size_t offset,
                                            int readLength1,
                                            int readLength2) {
    vector<vector<const Match *>> matchesForEachSpecies;
    TaxonScore bestScore;    
    vector<const Match *> curFrameMatches;
    vector<MatchPath> matchPaths;
    unordered_map<TaxID, float> species2score;
    unordered_map<TaxID, vector<MatchPath>> species2matchPaths;
    float bestSpScore = 0;
    unordered_map<TaxID, pair<size_t, size_t>> speciesMatchRange;
    
    size_t i = offset;
    while (i  < end + 1) {
        TaxID currentSpecies = matchList[i].speciesId;
        size_t start = i;
        // For current species
        while ((i < end + 1) && currentSpecies == matchList[i].speciesId) {
            uint8_t curFrame = matchList[i].qInfo.frame;
            curFrameMatches.clear();
            // For current frame
            while ((i < end + 1) && currentSpecies == matchList[i].speciesId && curFrame == matchList[i].qInfo.frame) {
                curFrameMatches.push_back(&matchList[i]);
                i ++;
            }
            if (curFrameMatches.size() > 1) {
                remainConsecutiveMatches(curFrameMatches, matchPaths, currentSpecies);
            }
        }
        speciesMatchRange[currentSpecies] = make_pair(start, i);
        // Combine MatchPaths
        if (!matchPaths.empty()) {
            // cout << currentSpecies << endl;
            float score = combineMatchPaths(matchPaths, species2matchPaths[currentSpecies], readLength1 + readLength2);
            // cout << endl;
            if (score > 1.0f) {score = 1.0f;}
            species2score[currentSpecies] = score;
            if (score > bestSpScore) {
                bestSpScore = score;
            }
        }
        matchPaths.clear();
    }
    
    // If there are no meaningful species
    if (species2score.empty()) {
        bestScore.score = 0;
        return bestScore;
    }

    vector<TaxID> maxSpecies;
    for (auto & spScore : species2score) {
        if (spScore.second > bestSpScore * 0.95) {
            maxSpecies.push_back(spScore.first);
        }
    }

    // More than one species --> LCA
    float coveredLength = 0.f;
    if (maxSpecies.size() > 1) {
        bestScore.LCA = true;
        bestScore.taxId = taxonomy->LCA(maxSpecies)->taxId;
        for (auto & sp : maxSpecies) {
            bestScore.score += species2score[sp];
            coveredLength = 0;
            for (auto & matchPath : species2matchPaths[maxSpecies[0]]) {
                coveredLength += matchPath.end - matchPath.start + 1;
            }
            bestScore.coverage += coveredLength / (readLength1 + readLength2);
        }
        bestScore.score /= maxSpecies.size();
        bestScore.coverage /= maxSpecies.size();
        return bestScore;
    }
    
    // One species
    bestScore.taxId = maxSpecies[0];
    bestScore.score = species2score[maxSpecies[0]];
    
    int hammingDist = 0;
    for (auto & matchPath : species2matchPaths[maxSpecies[0]]) {
        coveredLength += matchPath.end - matchPath.start + 1;
        hammingDist += matchPath.hammingDist;
    }
    speciesMatches.reserve(speciesMatchRange[bestScore.taxId].second
                        - speciesMatchRange[bestScore.taxId].first + 1);

    for (size_t i = speciesMatchRange[bestScore.taxId].first; i < speciesMatchRange[bestScore.taxId].second; i++) {
        speciesMatches.push_back(&matchList[i]);
    }
    bestScore.coverage = coveredLength / (readLength1 + readLength2);
    bestScore.hammingDist = hammingDist;

    return bestScore;                    
}

float Taxonomer::combineMatchPaths(vector<MatchPath> & matchPaths,
                                   vector<MatchPath> & combinedMatchPaths,
                                   int readLength) {
    combinedMatchPaths.clear();
    // Sort matchPaths by the their score
    sort(matchPaths.begin(), matchPaths.end(),
         [](const MatchPath & a, const MatchPath & b) { return a.score > b.score;});

    // Combine matchPaths
    // 1. Add the matchPath with the highest score to combinedMatchPaths
    // 2. Add the matchPath with the highest score that is not overlapped with the matchPath in combinedMatchPaths
    // 3. Repeat 2 until no matchPath can be added
    for (size_t i = 0; i < matchPaths.size(); i++) {
        if (combinedMatchPaths.empty()) {
            combinedMatchPaths.push_back(matchPaths[i]);
        } else {
            bool isOverlapped = false;
            for (size_t j = 0; j < combinedMatchPaths.size(); j++) {
                if (isMatchPathOverlapped(matchPaths[i], combinedMatchPaths[j])) { // overlap!
                    int overlappedLength = min(matchPaths[i].end, combinedMatchPaths[j].end) 
                                            - max(matchPaths[i].start, combinedMatchPaths[j].start) + 1;
                    if (overlappedLength < 24) {
                        trimMatchPath(matchPaths[i], combinedMatchPaths[j], overlappedLength);
                        continue;
                    } else {
                        isOverlapped = true;
                        break;
                    }
                    // if (isMatchPathLinked(matchPaths[i], combinedMatchPaths[j])) {
                    //     // merge two linked matchPaths by editing the combinedMatchPaths[j]
                    //     trimMatchPath(matchPaths[i], combinedMatchPaths[j]);                        
                    //     continue;
                    // } else {
                    //     isOverlapped = true;
                    //     break;
                    // }
                } 
            }
            if (!isOverlapped) {
                combinedMatchPaths.push_back(matchPaths[i]);
            }
        }
    }
    // Calculate the score of combinedMatchPaths
    float score = 0;
    for (auto & matchPath : combinedMatchPaths) {
        score += matchPath.score;
    }
    return score / readLength;
}

bool Taxonomer::isMatchPathLinked(const MatchPath & matchPath1, const MatchPath & matchPath2) {
    int overlappedLength = min(matchPath1.end, matchPath2.end) - max(matchPath1.start, matchPath2.start) + 1;
    if (20 >= overlappedLength || overlappedLength >= 24) {
        return false;
    }
    const Match * last;
    const Match * first;
    if (matchPath1.start < matchPath2.start) {
        last = matchPath1.matches.back();
        first = matchPath2.matches.front();
    } else {
        last = matchPath2.matches.back();
        first = matchPath1.matches.front();
    }
    if (overlappedLength == 21) {
        return isConsecutive(last, first);
    } else {

        return isConsecutive_diffFrame(last, first);
    }
    return false;
}

bool Taxonomer::isMatchPathOverlapped(const MatchPath & matchPath1,
                                      const MatchPath & matchPath2) {
    return !((matchPath1.end < matchPath2.start) || (matchPath2.end < matchPath1.start));                                       
}
// 87654321
void Taxonomer::mergeMatchPaths(const MatchPath & source, MatchPath & target) {
    if (source.start < target.start) {
        target.start = source.start;
        uint8_t lastEndHamming = GET_2_BITS(target.matches.front()->rightEndHamming);
        target.hammingDist += source.hammingDist - (source.matches.back()->hamming - lastEndHamming);
        target.score += source.score - source.matches.back()->getScore();
        if (lastEndHamming == 0) {
            target.score += 3.0f;
        } else {
            target.score += 2.0f - 0.5f * lastEndHamming;
        }
        target.matches.insert(target.matches.begin(), source.matches.begin(), source.matches.end() - 1);
    } else {
        target.end = source.end;
        uint8_t lastEndHamming = GET_2_BITS(source.matches.front()->rightEndHamming >> 14);
        target.hammingDist += source.hammingDist - (source.matches.front()->hamming - lastEndHamming);
        target.score += source.score - source.matches.front()->getScore();
        if (lastEndHamming == 0) {
            target.score += 3.0f;
        } else {
            target.score += 2.0f - 0.5f * lastEndHamming;
        }
        target.matches.insert(target.matches.end(), source.matches.begin() + 1, source.matches.end());
    }
}

void Taxonomer::trimMatchPath(MatchPath & path1, const MatchPath & path2, int overlapLength) {
    // int margin = min(path1.end, path2.end) - max(path1.start, path2.start) + 1 - 21;
    if (path1.start < path2.start) { 
        path1.end = path2.start - 1;
        uint8_t lastEndHamming = GET_2_BITS(path1.matches.back()->rightEndHamming);
        path1.hammingDist = path1.hammingDist - (path1.matches.back()->hamming - lastEndHamming);
        path1.score = path1.score - path1.matches.back()->getRightPartScore(overlapLength/3) - (overlapLength % 3);
        // if (lastEndHamming == 0) {
        //     path1.score += 3.0f;
        // } else {
        //     path1.score += 2.0f - 0.5f * lastEndHamming;
        // }
        // path1.matches.pop_back(); // unnecessary without checking isLikned
    } else {
        path1.start = path2.end + 1;
        uint8_t lastEndHamming = GET_2_BITS(path1.matches.front()->rightEndHamming >> 14);
        path1.hammingDist = path1.hammingDist - (path1.matches.front()->hamming - lastEndHamming);
        path1.score = path1.score - path1.matches.front()->getLeftPartScore(overlapLength/3) - (overlapLength % 3);
        // if (lastEndHamming == 0) {
        //     path1.score += 3.0f;
        // } else {
        //     path1.score += 2.0f - 0.5f * lastEndHamming;
        // }
        // path1.matches.erase(path1.matches.begin());
    }
}

void Taxonomer::remainConsecutiveMatches(const vector<const Match *> & curFrameMatches,
                                         vector<MatchPath> & matchPaths,
                                         TaxID genusId) {
    size_t i = 0;
    size_t end = curFrameMatches.size();
    vector<pair<const Match *, size_t>> curPosMatches; // <match, index>
    vector<pair<const Match *, size_t>> nextPosMatches;
    map<size_t, vector<size_t>> linkedMatches; // <index, linked indexes>

    size_t currPos = curFrameMatches[0]->qInfo.pos;
    while ( i < end && curFrameMatches[i]->qInfo.pos == currPos) {
        curPosMatches.emplace_back(curFrameMatches[i], i);
        i++;
    }
    while (i < end) {
        uint32_t nextPos = curFrameMatches[i]->qInfo.pos;
        while (i < end  && nextPos == curFrameMatches[i]->qInfo.pos) {
            nextPosMatches.emplace_back(curFrameMatches[i], i);
            ++ i;
        }
        // Check if current position and next position are consecutive
        if (currPos + 3 == nextPos) {
            // Compare curPosMatches and nextPosMatches
            for (auto &curPosMatch: curPosMatches) {
                for (auto &nextPosMatch: nextPosMatches) {
                    if (isConsecutive(curPosMatch.first, nextPosMatch.first)) {
                        linkedMatches[curPosMatch.second].push_back(nextPosMatch.second);
                    }
                }
            }

        }
        // Update curPosMatches and nextPosMatches
        curPosMatches = nextPosMatches;
        nextPosMatches.clear();
        currPos = nextPos;
    }
    // Print linkedMatches
//    if (par.printLog) {
//        cout << "linkedMatches: " << endl;
//        for (const auto &entry: linkedMatches) {
//            cout << entry.first << ": ";
//            for (auto &idx: entry.second) {
//                cout << idx << " ";
//            }
//            cout << endl;
//        }
//    }

    // Iterate linkedMatches to get filteredMatches 
    //(ignore matches not enoughly consecutive)
    size_t MIN_DEPTH = minConsCnt - 1;
    if (taxonomy->IsAncestor(eukaryotaTaxId, genusId)) {
        MIN_DEPTH = minConsCntEuk - 1;
    }
    unordered_set<size_t> used;
    unordered_map<size_t, depthScore> idx2depthScore;
    // unordered_map<size_t, size_t> edges;
    unordered_map<const Match *, const Match *> edges;

    for (const auto& entry : linkedMatches) {
        if (!used.count(entry.first)) {
            used.insert(entry.first);
            depthScore bestPath{};
            size_t bestNextIdx = 0;
            float curScore = curFrameMatches[entry.first]->getScore();
            // cout << curFrameMatches[entry.first]
            for (size_t j = 0; j < entry.second.size(); j++) {
                used.insert(entry.second[j]);
                depthScore curPath = DFS(curFrameMatches,
                                         entry.second[j],
                                         linkedMatches,
                                        1,
                                         MIN_DEPTH, 
                                         used, 
                                         idx2depthScore,
                                         edges, 
                                         curScore, 0);
                if (curPath.score > bestPath.score && curPath.depth > MIN_DEPTH) {
                    bestNextIdx = entry.second[j];
                    bestPath = curPath;
                }
            }
            // Store the best path
            if (bestPath.depth > MIN_DEPTH) {
                // cout << entry.first << endl;
                // curFrameMatches[entry.first]->printMatch();
                matchPaths.emplace_back(curFrameMatches[entry.first]->qInfo.pos, // start coordinate on query
                                        curFrameMatches[entry.first]->qInfo.pos + bestPath.depth * 3 + 20, // end coordinate on query
                                        bestPath.score, bestPath.hammingDist);
                const Match * curMatch = curFrameMatches[entry.first];
                edges[curMatch] = curFrameMatches[bestNextIdx];
                matchPaths.back().matches.push_back(curMatch);
                // curMatch = edges[curMatch];                        
                // edges2[curFrameMatches[entry.first]] = curFrameMatches[bestNextIdx];
                // Retrieve the best path
                // cout << bestPath.depth << endl;
                while (edges.find(curMatch) != edges.end()) {
                    // cout << curMatch << " ";
                    matchPaths.back().matches.push_back(edges[curMatch]);
                    curMatch = edges[curMatch];
                }
                // cout << endl;
            }
        }
    }
}

depthScore Taxonomer::DFS(const vector<const Match *> &matches,
                          size_t curMatchIdx,
                          const map<size_t, vector<size_t>> &linkedMatches,
                          size_t depth, size_t MIN_DEPTH,
                          unordered_set<size_t> &used,
                          unordered_map<size_t, depthScore> &idx2depthScore,
                          unordered_map<const Match *, const Match *> & edges, float score, int hammingDist) {
    depth++;
    depthScore bestDepthScore = depthScore(0, 0, 0);
    depthScore returnDepthScore;
    depthScore curDepthScore;
    float recievedScore = score;
    if (linkedMatches.find(curMatchIdx) == linkedMatches.end()) { // reached a leaf node
        uint8_t lastEndHamming = (matches[curMatchIdx]->rightEndHamming >> 14);
        if (lastEndHamming == 0) {
            score += 3.0f;
        } else {
            score += 2.0f - 0.5f * lastEndHamming;
        }
        idx2depthScore[curMatchIdx] = depthScore(1, score - recievedScore, lastEndHamming);
        return depthScore(depth, score, hammingDist + lastEndHamming);
    } else { // not a leaf node
        uint8_t lastEndHamming = (matches[curMatchIdx]->rightEndHamming >> 14);
        if (lastEndHamming == 0) {
            score += 3.0f;
        } else {
            score += 2.0f - 0.5f * lastEndHamming;
        }
        for (auto &nextMatchIdx: linkedMatches.at(curMatchIdx)) {
            used.insert(nextMatchIdx);
            // Reuse the depth score of nextMatchIdx if it has been calculated
            if (idx2depthScore.find(nextMatchIdx) != idx2depthScore.end()){
                returnDepthScore = idx2depthScore[nextMatchIdx];
                curDepthScore = depthScore(returnDepthScore.depth + depth,
                                           returnDepthScore.score + score,
                                           returnDepthScore.hammingDist + hammingDist + lastEndHamming);
            } else {
                curDepthScore = DFS(matches, nextMatchIdx, linkedMatches, depth, MIN_DEPTH, used, idx2depthScore, edges, score, hammingDist + lastEndHamming);
            }
            if (curDepthScore.score > bestDepthScore.score
                && curDepthScore.depth > MIN_DEPTH) {
                bestDepthScore = curDepthScore;
                edges[matches[curMatchIdx]] = matches[nextMatchIdx];
            }
        }    
        if (bestDepthScore.depth > MIN_DEPTH) {
            idx2depthScore[curMatchIdx] = depthScore(bestDepthScore.depth - depth + 1,
                                                     bestDepthScore.score - recievedScore,
                                                     bestDepthScore.hammingDist - hammingDist);
        }
    }
    return bestDepthScore;
}

TaxonScore Taxonomer::scoreTaxon(vector<const Match *> &filteredMatches,
                                 TaxID taxId,
                                 int queryLength) {
    // Calculate Hamming distance & covered length
    int coveredPosCnt = 0;
    uint16_t currHammings;
    int aminoAcidNum = (int) queryLength / 3;
    int currPos;
    size_t matchNum = filteredMatches.size();
    size_t f = 0;

    // Get the smallest hamming distance at each position of query
    auto *hammingsAtEachPos = new signed char[aminoAcidNum + 1];
    memset(hammingsAtEachPos, 24, (aminoAcidNum + 1));
    while (f < matchNum) {
        currPos = filteredMatches[f]->qInfo.pos / 3;
        currHammings = filteredMatches[f]->rightEndHamming;
        if (GET_2_BITS(currHammings) < hammingsAtEachPos[currPos + unmaskedPos[0]])
            hammingsAtEachPos[currPos + unmaskedPos[0]] = GET_2_BITS(currHammings);
        if (GET_2_BITS(currHammings >> 2) < hammingsAtEachPos[currPos + unmaskedPos[1]])
            hammingsAtEachPos[currPos + unmaskedPos[1]] = GET_2_BITS(currHammings >> 2);
        if (GET_2_BITS(currHammings >> 4) < hammingsAtEachPos[currPos + unmaskedPos[2]])
            hammingsAtEachPos[currPos + unmaskedPos[2]] = GET_2_BITS(currHammings >> 4);
        if (GET_2_BITS(currHammings >> 6) < hammingsAtEachPos[currPos + unmaskedPos[3]])
            hammingsAtEachPos[currPos + unmaskedPos[3]] = GET_2_BITS(currHammings >> 6);
        if (GET_2_BITS(currHammings >> 8) < hammingsAtEachPos[currPos + unmaskedPos[4]])
            hammingsAtEachPos[currPos + unmaskedPos[4]] = GET_2_BITS(currHammings >> 8);
        if (GET_2_BITS(currHammings >> 10) < hammingsAtEachPos[currPos + unmaskedPos[5]])
            hammingsAtEachPos[currPos + unmaskedPos[5]] = GET_2_BITS(currHammings >> 10);
        if (GET_2_BITS(currHammings >> 12) < hammingsAtEachPos[currPos + unmaskedPos[6]])
            hammingsAtEachPos[currPos + unmaskedPos[6]] = GET_2_BITS(currHammings >> 12);
        if (GET_2_BITS(currHammings >> 14) < hammingsAtEachPos[currPos + unmaskedPos[7]])
            hammingsAtEachPos[currPos + unmaskedPos[7]] = GET_2_BITS(currHammings >> 14);
        f++;
    }

    // Sum up hamming distances and count the number of position covered by the matches.
    float hammingSum = 0;
    for (int h = 0; h < aminoAcidNum; h++) {
        if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
            coveredPosCnt++;
        } else if (hammingsAtEachPos[h] != 24) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
            hammingSum += 1.0f + (0.5f * hammingsAtEachPos[h]);
            coveredPosCnt++;
        }
    }
    delete[] hammingsAtEachPos;

    // Score current genus
    int coveredLength = coveredPosCnt * 3;
    if (coveredLength > queryLength) coveredLength = queryLength;
    float score = ((float) coveredLength - hammingSum) / (float) queryLength;
    float coverage = (float) (coveredLength) / (float) (queryLength);

    return {taxId, score, coverage, (int) hammingSum, 0};
}

TaxonScore Taxonomer::scoreTaxon(vector<const Match *> &filteredMatches,
                                 TaxID taxId,       
                                 int readLength1,
                                 int readLength2) {

    // Calculate Hamming distance & covered length
    int aminoAcidNum_total = ((int) readLength1 / 3) + ((int) readLength2 / 3);
    int aminoAcidNum_read1 = ((int) readLength1 / 3);
    int currPos;
    size_t matchNum = filteredMatches.size();
    size_t f = 0;

    // Get the smallest hamming distance at each position of query
    auto *hammingsAtEachPos = new signed char[aminoAcidNum_total + 3];
    memset(hammingsAtEachPos, 24, (aminoAcidNum_total + 3));
    while (f < matchNum) {
        uint8_t minHammingDist = 24;
        uint16_t currHammings = 0;
        currPos = (int) filteredMatches[f]->qInfo.pos / 3;
        // Find the closest match at current position
        while ((f < matchNum) && currPos == (int) filteredMatches[f]->qInfo.pos / 3) {
            if (filteredMatches[f]->hamming < minHammingDist) {
                minHammingDist = filteredMatches[f]->hamming;
                currHammings = filteredMatches[f]->rightEndHamming;
            }
            f++;
        }
        // Update hamming distance at each position
        if (GET_2_BITS(currHammings) < hammingsAtEachPos[currPos + unmaskedPos[0]])
            hammingsAtEachPos[currPos + unmaskedPos[0]] = GET_2_BITS(currHammings);
        if (GET_2_BITS(currHammings >> 2) < hammingsAtEachPos[currPos + unmaskedPos[1]])
            hammingsAtEachPos[currPos + unmaskedPos[1]] = GET_2_BITS(currHammings >> 2);
        if (GET_2_BITS(currHammings >> 4) < hammingsAtEachPos[currPos + unmaskedPos[2]])
            hammingsAtEachPos[currPos + unmaskedPos[2]] = GET_2_BITS(currHammings >> 4);
        if (GET_2_BITS(currHammings >> 6) < hammingsAtEachPos[currPos + unmaskedPos[3]])
            hammingsAtEachPos[currPos + unmaskedPos[3]] = GET_2_BITS(currHammings >> 6);
        if (GET_2_BITS(currHammings >> 8) < hammingsAtEachPos[currPos + unmaskedPos[4]])
            hammingsAtEachPos[currPos + unmaskedPos[4]] = GET_2_BITS(currHammings >> 8);
        if (GET_2_BITS(currHammings >> 10) < hammingsAtEachPos[currPos + unmaskedPos[5]])
            hammingsAtEachPos[currPos + unmaskedPos[5]] = GET_2_BITS(currHammings >> 10);
        if (GET_2_BITS(currHammings >> 12) < hammingsAtEachPos[currPos + unmaskedPos[6]])
            hammingsAtEachPos[currPos + unmaskedPos[6]] = GET_2_BITS(currHammings >> 12);
        if (GET_2_BITS(currHammings >> 14) < hammingsAtEachPos[currPos + unmaskedPos[7]])
            hammingsAtEachPos[currPos + unmaskedPos[7]] = GET_2_BITS(currHammings >> 14);
        f++;
    }

    // Sum up hamming distances and count the number of position covered by the matches.
    float hammingSum = 0;
    int coveredPosCnt_read1 = 0;
    int coveredPosCnt_read2 = 0;
    for (int h = 0; h < aminoAcidNum_total; h++) {
        // Read 1
        if (h < aminoAcidNum_read1) {
            if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
                coveredPosCnt_read1++;
            } else if (hammingsAtEachPos[h] != 24) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
                hammingSum += 1.0f + (0.5f * (float) hammingsAtEachPos[h]);
                coveredPosCnt_read1++;
            }
        }
            // Read 2
        else {
            if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
                coveredPosCnt_read2++;
            } else if (hammingsAtEachPos[h] != 24) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
                hammingSum += 1.0f + (0.5f * (float) hammingsAtEachPos[h]);
                coveredPosCnt_read2++;
            }
        }
    }
    delete[] hammingsAtEachPos;

    // Score current genus
    int coveredLength_read1 = coveredPosCnt_read1 * 3;
    int coveredLength_read2 = coveredPosCnt_read2 * 3;
    if (coveredLength_read1 > readLength1) coveredLength_read1 = readLength1;
    if (coveredLength_read2 > readLength2) coveredLength_read2 = readLength2;
    float score =
            ((float) (coveredLength_read1 + coveredLength_read2) - hammingSum) / (float) (readLength1 + readLength2);
    float coverage = (float) (coveredLength_read1 + coveredLength_read2) / (float) (readLength1 + readLength2);

//    matchesForEachGenus.push_back(move(filteredMatches));
    return {taxId, score, coverage, (int) hammingSum, 0};
}

TaxonScore Taxonomer::chooseSpecies(const vector<Match> &matches,
                                     int queryLength,
                                     vector<TaxID> &species,
                                     unordered_map<TaxID, pair<int, int>> & speciesMatchRange) {
    // Score each species
    std::unordered_map<TaxID, TaxonScore> speciesScores;
    size_t i = 0;
    TaxID currentSpeices;
    size_t numOfMatch = matches.size();
    size_t speciesBegin, speciesEnd;
    while (i < numOfMatch) {
        currentSpeices = matches[i].speciesId;
        speciesBegin = i;
        while ((i < numOfMatch) && currentSpeices == matches[i].speciesId) {
            i++;
        }
        speciesEnd = i;
        speciesScores[currentSpeices] = scoreSpecies(matches, speciesBegin, speciesEnd, queryLength);
        speciesMatchRange[currentSpeices] = {(int) speciesBegin, (int) speciesEnd};
        speciesScores[currentSpeices].taxId = currentSpeices;
    }

    // Get the best species
    TaxonScore bestScore;
    for (auto & sp : speciesScores) {
        if (sp.second.score > bestScore.score) {
            species.clear();
            species.push_back(sp.first);
            bestScore = sp.second;
        } else if (sp.second.coverage == bestScore.coverage) {
            species.push_back(sp.first);
        }
    }
    return bestScore;
}

TaxonScore Taxonomer::chooseSpecies(const vector<Match> &matches,
                                     int read1Length,
                                     int read2Length,
                                     vector<TaxID> &species,
                                     unordered_map<TaxID, pair<int, int>> & speciesMatchRange) {
    // Score each species
    std::unordered_map<TaxID, TaxonScore> speciesScores;

    size_t i = 0;
    TaxID currentSpeices;
    size_t numOfMatch = matches.size();
    size_t speciesBegin, speciesEnd;
    while (i < numOfMatch) {
        currentSpeices = matches[i].speciesId;
        speciesBegin = i;
        while ((i < numOfMatch) && currentSpeices == matches[i].speciesId) {
            i++;
        }
        speciesEnd = i;
        speciesScores[currentSpeices] = scoreSpecies(matches, speciesBegin, speciesEnd, read1Length, read2Length);
        speciesMatchRange[currentSpeices] = {(int) speciesBegin, (int) speciesEnd};
        speciesScores[currentSpeices].taxId = currentSpeices;
    }

    // Get the best species
    TaxonScore bestScore;
    for (auto & sp : speciesScores) {
        if (sp.second.score > bestScore.score) {
            species.clear();
            species.push_back(sp.first);
            bestScore = sp.second;
        } else if (sp.second.coverage == bestScore.coverage) {
            species.push_back(sp.first);
        }
    }
    return bestScore;
}

TaxonScore Taxonomer::scoreSpecies(const vector<Match> &matches,
                                    size_t begin,
                                    size_t end,
                                    int queryLength) {

    // Get the largest hamming distance at each position of query
    int aminoAcidNum = queryLength / 3;
    auto *hammingsAtEachPos = new signed char[aminoAcidNum + 1];
    memset(hammingsAtEachPos, -1, (aminoAcidNum + 1));
    int currPos;
    size_t walker = begin;
    uint16_t currHammings;
    while (walker < end) {
        currPos = matches[walker].qInfo.pos / 3;
        currHammings = matches[walker].rightEndHamming;
        if (GET_2_BITS(currHammings) > hammingsAtEachPos[currPos + unmaskedPos[0]])
            hammingsAtEachPos[currPos + unmaskedPos[0]] = GET_2_BITS(currHammings);
        if (GET_2_BITS(currHammings >> 2) > hammingsAtEachPos[currPos + unmaskedPos[1]])
            hammingsAtEachPos[currPos + unmaskedPos[1]] = GET_2_BITS(currHammings >> 2);
        if (GET_2_BITS(currHammings >> 4) > hammingsAtEachPos[currPos + unmaskedPos[2]])
            hammingsAtEachPos[currPos + unmaskedPos[2]] = GET_2_BITS(currHammings >> 4);
        if (GET_2_BITS(currHammings >> 6) > hammingsAtEachPos[currPos + unmaskedPos[3]])
            hammingsAtEachPos[currPos + unmaskedPos[3]] = GET_2_BITS(currHammings >> 6);
        if (GET_2_BITS(currHammings >> 8) > hammingsAtEachPos[currPos + unmaskedPos[4]])
            hammingsAtEachPos[currPos + unmaskedPos[4]] = GET_2_BITS(currHammings >> 8);
        if (GET_2_BITS(currHammings >> 10) > hammingsAtEachPos[currPos + unmaskedPos[5]])
            hammingsAtEachPos[currPos + unmaskedPos[5]] = GET_2_BITS(currHammings >> 10);
        if (GET_2_BITS(currHammings >> 12) > hammingsAtEachPos[currPos + unmaskedPos[6]])
            hammingsAtEachPos[currPos + unmaskedPos[6]] = GET_2_BITS(currHammings >> 12);
        if (GET_2_BITS(currHammings >> 14) > hammingsAtEachPos[currPos + unmaskedPos[7]])
            hammingsAtEachPos[currPos + unmaskedPos[7]] = GET_2_BITS(currHammings >> 14);
        walker++;
    }

    // Sum up hamming distances and count the number of position covered by the matches.
    float hammingSum = 0;
    int hammingDist = 0;
    int coveredPosCnt = 0;
    for (int h = 0; h < aminoAcidNum; h++) {
        if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
            coveredPosCnt++;
        } else if (hammingsAtEachPos[h] != -1) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
            hammingSum += 1.0f + (0.5f * (float) hammingsAtEachPos[h]);
            hammingDist += hammingsAtEachPos[h];
            coveredPosCnt++;
        }
    }
    delete[] hammingsAtEachPos;
    // Score
    int coveredLength = coveredPosCnt * 3;
    if (coveredLength >= queryLength) coveredLength = queryLength;

    float score = ((float)coveredLength - hammingSum) / (float) queryLength;
    float coverage = (float) coveredLength / (float) (queryLength);

    return {0, score, coverage, hammingDist, 0};
}

TaxonScore Taxonomer::scoreSpecies(const vector<Match> &matches,
                                    size_t begin,
                                    size_t end,
                                    int queryLength,
                                    int queryLength2) {

    // Get the largest hamming distance at each position of query
    int aminoAcidNum_total = queryLength / 3 + queryLength2 / 3;
    int aminoAcidNum_read1 = queryLength / 3;
    auto *hammingsAtEachPos = new signed char[aminoAcidNum_total + 3];
    memset(hammingsAtEachPos, -1, (aminoAcidNum_total + 3));

    int currPos;
    size_t walker = begin;
    uint16_t currHammings;

    while (walker < end) {
        currPos = matches[walker].qInfo.pos / 3;
        currHammings = matches[walker].rightEndHamming;
        if (GET_2_BITS(currHammings) > hammingsAtEachPos[currPos + unmaskedPos[0]])
            hammingsAtEachPos[currPos + unmaskedPos[0]] = GET_2_BITS(currHammings);
        if (GET_2_BITS(currHammings >> 2) > hammingsAtEachPos[currPos + unmaskedPos[1]])
            hammingsAtEachPos[currPos + unmaskedPos[1]] = GET_2_BITS(currHammings >> 2);
        if (GET_2_BITS(currHammings >> 4) > hammingsAtEachPos[currPos + unmaskedPos[2]])
            hammingsAtEachPos[currPos + unmaskedPos[2]] = GET_2_BITS(currHammings >> 4);
        if (GET_2_BITS(currHammings >> 6) > hammingsAtEachPos[currPos + unmaskedPos[3]])
            hammingsAtEachPos[currPos + unmaskedPos[3]] = GET_2_BITS(currHammings >> 6);
        if (GET_2_BITS(currHammings >> 8) > hammingsAtEachPos[currPos + unmaskedPos[4]])
            hammingsAtEachPos[currPos + unmaskedPos[4]] = GET_2_BITS(currHammings >> 8);
        if (GET_2_BITS(currHammings >> 10) > hammingsAtEachPos[currPos + unmaskedPos[5]])
            hammingsAtEachPos[currPos + unmaskedPos[5]] = GET_2_BITS(currHammings >> 10);
        if (GET_2_BITS(currHammings >> 12) > hammingsAtEachPos[currPos + unmaskedPos[6]])
            hammingsAtEachPos[currPos + unmaskedPos[6]] = GET_2_BITS(currHammings >> 12);
        if (GET_2_BITS(currHammings >> 14) > hammingsAtEachPos[currPos + unmaskedPos[7]])
            hammingsAtEachPos[currPos + unmaskedPos[7]] = GET_2_BITS(currHammings >> 14);
        walker++;
    }

    // Sum up hamming distances and count the number of position covered by the matches.
    float hammingSum = 0;
    int hammingDist = 0;
    int coveredPosCnt_read1 = 0;
    int coveredPosCnt_read2 = 0;
    for (int h = 0; h < aminoAcidNum_total; h++) {
        // Read 1
        if (h < aminoAcidNum_read1) {
            if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
                coveredPosCnt_read1++;
            } else if (hammingsAtEachPos[h] != -1) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
                hammingSum += 1.0f + (0.5f * (float) hammingsAtEachPos[h]);
                hammingDist += hammingsAtEachPos[h];
                coveredPosCnt_read1++;
            }
        }
            // Read 2
        else {
            if (hammingsAtEachPos[h] == 0) { // Add 0 for 0 hamming dist.
                coveredPosCnt_read2++;
            } else if (hammingsAtEachPos[h] != -1) { // Add 1.5, 2, 2.5 for 1, 2, 3 hamming dist. respectively
                hammingSum += 1.0f + (0.5f * (float) hammingsAtEachPos[h]);
                hammingDist += hammingsAtEachPos[h];
                coveredPosCnt_read2++;
            }
        }
    }
    delete[] hammingsAtEachPos;

    // Score
    int coveredLength_read1 = coveredPosCnt_read1 * 3;
    int coveredLength_read2 = coveredPosCnt_read2 * 3;
    if (coveredLength_read1 >= queryLength) coveredLength_read1 = queryLength;
    if (coveredLength_read2 >= queryLength2) coveredLength_read2 = queryLength2;

    float score = ((float) (coveredLength_read1 + coveredLength_read2) - hammingSum) / (float) (queryLength + queryLength2);
    float coverage = (float) (coveredLength_read1 + coveredLength_read2) / (float) (queryLength + queryLength2);

    return {0, score, coverage, hammingDist, 0};
}

bool Taxonomer::isConsecutive(const Match * match1, const Match * match2) {
    // match1 87654321 -> 08765432
    // match2 98765432 -> 08765432
    return (match1->rightEndHamming >> 2) == (match2->rightEndHamming & 0x3FFF);
}

bool Taxonomer::isConsecutive_diffFrame(const Match * match1, const Match * match2) {
    // int hamming1 = match1->hamming - GET_2_BITS(match1->rightEndHamming);
    // int hamming2 = match2->hamming - GET_2_BITS(match2->rightEndHamming >> 14);
    // cout << match1->rightEndHamming << " " << match2->rightEndHamming << endl;
    // cout << hamming1 << " " << hamming2 << endl;
    // match1 87654321 -> 08765432
    // match2 98765432 -> 08765432
    return (match1->hamming - GET_2_BITS(match1->rightEndHamming)) == (match2->hamming - GET_2_BITS(match2->rightEndHamming >> 14));
}