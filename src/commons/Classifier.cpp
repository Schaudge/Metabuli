//
// Created by KJB on 01/09/2020.
//

#include "Classifier.h"

Classifier::Classifier() {
    seqIterator = new SeqIterator();
    numOfSplit = 0;
    closestCount = 0;
    queryCount = 0;
}

Classifier::~Classifier() { delete seqIterator; }

void Classifier::startClassify(const char * queryFileName, const char * targetDiffIdxFileName, const char * targetInfoFileName, vector<int> & taxIdList) {

    NcbiTaxonomy ncbiTaxonomy("/Users/jaebeomkim/Desktop/pjt/taxdmp/names.dmp",
                              "/Users/jaebeomkim/Desktop/pjt/taxdmp/nodes.dmp",
                              "/Users/jaebeomkim/Desktop/pjt/taxdmp/merged.dmp");

    vector<int> taxIdListAtRank;
    ncbiTaxonomy.makeTaxIdListAtRank(taxIdList, taxIdListAtRank, "species");

    struct MmapedData<char> queryFile = mmapData<char>(queryFileName);
    struct MmapedData<uint16_t> targetDiffIdxList = mmapData<uint16_t>(targetDiffIdxFileName);
    targetDiffIdxList.data[targetDiffIdxList.fileSize/sizeof(uint16_t)] = 32768; //1000000000000000
    struct MmapedData<TargetKmerInfo> targetInfoList = mmapData<TargetKmerInfo>(targetInfoFileName);

    vector<Sequence> sequences;
    seqIterator->getSeqSegmentsWithHead(sequences, queryFile);
    size_t numOfSeq = sequences.size();

    bool processedSeqChecker[numOfSeq];
    fill_n(processedSeqChecker, numOfSeq, false);

    QueryKmerBuffer kmerBuffer(kmerBufSize);
    size_t processedSeqCnt;

    cout<<"numOfseq: "<<numOfSeq<<endl;
    while(processedSeqCnt < numOfSeq){ ///check this condition
        fillQueryKmerBufferParallel(kmerBuffer, queryFile, sequences, processedSeqChecker, processedSeqCnt);
        linearSearch(kmerBuffer.buffer, kmerBuffer.startIndexOfReserve, targetDiffIdxList, targetInfoList, taxIdList, taxIdListAtRank);
    }

    cout<<"Number of query k-mer                : "<<queryCount<<endl;
    cout<<"Number of total match                : "<<totalMatchCount <<endl;
    cout<<"mutipleMatch in AA level             : "<<multipleMatchCount << endl;
    cout<<"matches in DNA level                 : "<<perfectMatchCount<<endl;
    cout<<"number of closest matches            : "<<closestCount<<endl;

    analyseResult(ncbiTaxonomy, sequences);

    free(kmerBuffer.buffer);
    munmap(queryFile.data, queryFile.fileSize + 1);
    munmap(targetDiffIdxList.data, targetDiffIdxList.fileSize + 1);
    munmap(targetInfoList.data, targetInfoList.fileSize + 1);
}

void Classifier::fillQueryKmerBufferParallel(QueryKmerBuffer & kmerBuffer, MmapedData<char> & seqFile, vector<Sequence> & seqs, bool * checker, size_t & processedSeqCnt) {
#ifdef OPENMP
    omp_set_num_threads(1);
#endif

#pragma omp parallel
    {
        SeqIterator seqIterator;
        size_t posToWrite;
        bool hasOverflow = false;
#pragma omp for schedule(dynamic, 1)
        for (size_t i = 0; i < seqs.size(); i++) {
            if(checker[i] == false && !hasOverflow) {
                kseq_buffer_t buffer(const_cast<char *>(&seqFile.data[seqs[i].start]), seqs[i].length);
                kseq_t *seq = kseq_init(&buffer);
                kseq_read(seq);
                seqs[i].length = strlen(seq->seq.s);

                seqIterator.sixFrameTranslation(seq->seq.s);

                size_t kmerCnt = seqIterator.KmerNumOfSixFrameTranslation(seq->seq.s);
                posToWrite = kmerBuffer.reserveMemory(kmerCnt);
                if (posToWrite + kmerCnt < kmerBufSize) {
                    seqIterator.fillQueryKmerBuffer(seq->seq.s, kmerBuffer, posToWrite, i);
                    checker[i] = true;
                    processedSeqCnt ++;
                } else{
                    kmerBuffer.startIndexOfReserve -= kmerCnt;
                    hasOverflow = true;
                }
            }

        }

    }
    return;
}

///It compares query k-mers to target k-mers. If a query has matches, the matches with the smallest difference are selected.
void Classifier::linearSearch(QueryKmer * queryKmerList, size_t & numOfQuery, const MmapedData<uint16_t> & targetDiffIdxList, const MmapedData<TargetKmerInfo> & targetInfoList, const vector<int> & taxIdList, const vector<int> & taxIdListAtRank) {
    ///initialize
    size_t diffIdxPos = 0;
    uint64_t lastFirstMatch = 0;
    long lastFirstDiffIdxPos = 0;
    int lastFirstTargetIdx = 0;

    size_t numOfTargetKmer = targetInfoList.fileSize / sizeof(TargetKmerInfo);
    uint8_t lowestHamming;
    SORT_PARALLEL(queryKmerList, queryKmerList + numOfQuery , Classifier::compareForLinearSearch);
    uint64_t nextTargetKmer = getNextTargetKmer(0, targetDiffIdxList.data, diffIdxPos);
    size_t tarIter = 0;

    uint64_t currentQuery = UINT64_MAX;
    uint64_t currentTargetKmer = UINT64_MAX;
    uint64_t currentQueryAA;
    vector<int> hammings;

    for(size_t i = 0; i < numOfQuery; i++){
        /// get next query
        if(AminoAcid(currentQuery) == AminoAcid(queryKmerList[i].ADkmer)){
            nextTargetKmer = lastFirstMatch;
            diffIdxPos = lastFirstDiffIdxPos;
            tarIter = lastFirstTargetIdx;
        }
        currentQuery = queryKmerList[i].ADkmer;
        isMatched = 0;
        lowestHamming = 100;
        queryCount ++;

        currentQueryAA = AminoAcid(currentQuery);

        while((tarIter < numOfTargetKmer) && (AminoAcid(nextTargetKmer) <= currentQueryAA)){
            currentTargetKmer = nextTargetKmer;
            currentTargetPos = diffIdxPos;
            nextTargetKmer = getNextTargetKmer(currentTargetKmer, targetDiffIdxList.data, diffIdxPos);
          //  seqIterator->printKmerInDNAsequence(nextTargetKmer);
            if(currentQuery == currentTargetKmer){
                perfectMatchCount ++;
//                cout<<taxIdList[targetInfoList.data[tarIter].sequenceID]<<" "<<targetInfoList.data[tarIter].sequenceID<<endl;
//                cout<<"query : "; seqIterator->printKmerInDNAsequence(currentQuery); cout<<endl;
//                cout<<"target: "; seqIterator->printKmerInDNAsequence(currentTargetKmer); cout<<endl;
            }

            if(AminoAcid(currentTargetKmer) == AminoAcid(currentQuery)){
                if (isMatched == 0) {
                    lastFirstMatch = currentTargetKmer;
                    lastFirstDiffIdxPos = currentTargetPos;
                    lastFirstTargetIdx = tarIter;
                    isMatched = 1;
                }
                totalMatchCount++;

                currentHamming = getHammingDistance(currentQuery, currentTargetKmer);
                hammings.push_back(currentHamming);
                closestKmers.push_back(tarIter);

                if(currentHamming < lowestHamming){
                    lowestHamming = currentHamming;
                }

//                if(currentHamming > lowestHamming + 1){
//                    tarIter ++;
//                    continue;
//                } else if(currentHamming < lowestHamming){
//                    closestKmers.clear();
//                    lowestHamming = currentHamming;
//                }
//                 if(currentHamming == lowestHamming) closestKmers.push_back(tarIter);

//                if(currentHamming > lowestHamming){
//                    tarIter ++;
//                    continue;
//                } else if(currentHamming < lowestHamming){
//                    closestKmersCnt = (currentHamming == lowestHamming) ? closestKmersCnt : 0;
//                    closestKmers[closestKmersCnt] = tarIter;
//                    lowestHamming = currentHamming;
//                    closestKmersCnt++;
//
//                }

            }
            tarIter ++;
        }

        for(size_t k = 0; k < closestKmers.size(); k++){
            ///TODO generous hamming?
            if(hammings[k] == lowestHamming){
                if(targetInfoList.data[closestKmers[k]].redundancy == true) {
                    matchedKmerList.emplace_back(queryKmerList[i].info.sequenceID,
                                                 targetInfoList.data[closestKmers[k]].sequenceID,
                                                 taxIdListAtRank[targetInfoList.data[closestKmers[k]].sequenceID],
                                                 queryKmerList[i].info.pos, hammings[k],
                                                 targetInfoList.data[closestKmers[k]].redundancy,
                                                 queryKmerList[i].info.frame);
                }else{
                    matchedKmerList.emplace_back(queryKmerList[i].info.sequenceID,
                                                 targetInfoList.data[closestKmers[k]].sequenceID,
                                                 taxIdList[targetInfoList.data[closestKmers[k]].sequenceID],
                                                 queryKmerList[i].info.pos, hammings[k],
                                                 targetInfoList.data[closestKmers[k]].redundancy,
                                                 queryKmerList[i].info.frame);
                }
                closestCount++;
            }
//            matchedKmerList.emplace_back(queryKmerList[i].info.sequenceID, targetInfoList.data[closestKmers[k]].sequenceID, taxIdList[targetInfoList.data[closestKmers[k]].sequenceID],
//                                         queryKmerList[i].info.pos, lowestHamming, targetInfoList.data[closestKmers[k]].redundancy);

        }
        closestKmers.clear();
        hammings.clear();
    }
    numOfQuery = 0;
}

TaxID Classifier::lca2taxon(unordered_map<TaxID, int> & listOfLCAs, NcbiTaxonomy & ncbiTaxonomy, const size_t & length, float coverageThr){
    float numberOfPossibleKmersFromThisRead = length/3 - 7;
    unordered_map<TaxID, float> taxonNodeCount; //int = count
    double totalCount = 0;
    TaxID currTaxId;
    float currCount;
    int highestRankSatisfyingThr = 0;
    int maxCount = 0;
    int currRank;
    TaxID selectedLeaf = 0;
    coverageThr = 0.5; ///TODO: Optimize
    float maxCoverage = 0;
    float currCoverage;
    vector<int> strainList;
    int isClassified = 0;

    for(unordered_map<TaxID, int>::iterator it = listOfLCAs.begin(); it != listOfLCAs.end(); ++it) {
        currTaxId = it->first;
        currCount = it->second;
        for (std::pair<TaxID, int> it2 : listOfLCAs) {
            if ((it2.first != currTaxId) && ncbiTaxonomy.IsAncestor(it2.first, currTaxId)) {
                currCount += it2.second;
            }
        }
        cout<<"count of leaf: "<<currTaxId<< " "<<currCount<<" "<<((float)currCount)/numberOfPossibleKmersFromThisRead<<" "<<NcbiTaxonomy::findRankIndex(ncbiTaxonomy.taxonNode(currTaxId)->rank)<<endl;
        taxonNodeCount.insert(pair<TaxID, float>(currTaxId, currCount));

        totalCount += it->second;

    }

    for(auto it = taxonNodeCount.begin(); it != taxonNodeCount.end(); ++it) {

        currCoverage = it->second / numberOfPossibleKmersFromThisRead;
        if (currCoverage >= coverageThr){
            currRank = NcbiTaxonomy::findRankIndex(ncbiTaxonomy.taxonNode(it->first)->rank);
            if(currRank < 4){
                strainList.push_back(it->first);
            }
            if((currRank > highestRankSatisfyingThr) || (currRank == highestRankSatisfyingThr && currCoverage > maxCoverage )) {
                highestRankSatisfyingThr = currRank;
                maxCount = it->second;
                selectedLeaf = it->first;
                isClassified = 1;
                cout<<"highestRank: "<<highestRankSatisfyingThr<<" "<<"maxCount: "<<maxCount<<" "<<"selected leaf: "<<selectedLeaf<<endl;
            }
        }
    }


    ///TODO check here
    int maxStrainCnt = 0;
    if(isClassified && NcbiTaxonomy::findRankIndex(ncbiTaxonomy.taxonNode(selectedLeaf)->rank) == 4) {
        for (int st = 0; st < strainList.size(); st++) {
            if ((taxonNodeCount[strainList[st]] > 1.1 * maxCount) && (taxonNodeCount[strainList[st]] / numberOfPossibleKmersFromThisRead > 0.9)) {
                if(taxonNodeCount[strainList[st]] > maxStrainCnt){
                    maxStrainCnt = taxonNodeCount[strainList[st]];
                    selectedLeaf = strainList[st];
                    cout << "here: " << taxonNodeCount[strainList[st]] << endl;
                }
            }
        }
    }
    cout<<"LEAF: "<<selectedLeaf<<endl<<endl;
    return selectedLeaf; // 0 -> unclassified
}

TaxID Classifier::match2LCA(const std::vector<int> & taxIdList, NcbiTaxonomy const & taxonomy, const float majorityCutoff,
                            size_t &numAssignedSeqs, size_t &numUnassignedSeqs, size_t &numSeqsAgreeWithSelectedTaxon, double &selectedPercent){
    std::map<TaxID,taxNode> ancTaxIdsCounts;

    numAssignedSeqs = 0;
    numUnassignedSeqs = 0;
    numSeqsAgreeWithSelectedTaxon = 0;
    selectedPercent = 0;
    double totalAssignedSeqsWeights = 0.0;

    for (size_t i = 0; i < taxIdList.size(); ++i) {
        TaxID currTaxId = taxIdList[i];
        double currWeight = 1;
        // ignore unassigned sequences
        if (currTaxId == 0) {
            numUnassignedSeqs++;
            continue;
        }
        TaxonNode const * node = taxonomy.taxonNode(currTaxId, false);
        if (node == NULL) {
            Debug(Debug::ERROR) << "taxonid: " << currTaxId << " does not match a legal taxonomy node.\n";
            EXIT(EXIT_FAILURE);
        }
        totalAssignedSeqsWeights += currWeight;
        numAssignedSeqs++;

        // each start of a path due to an orf is a candidate
        if (ancTaxIdsCounts.find(currTaxId) != ancTaxIdsCounts.end()) { //원소가 있다면
            ancTaxIdsCounts[currTaxId].update(currWeight, 0);
        } else {
            taxNode currNode;
            currNode.set(currWeight, true, 0);
            ancTaxIdsCounts.insert(std::pair<TaxID,taxNode>(currTaxId, currNode));
        }

        // iterate all ancestors up to root (including). add currWeight and candidate status to each
        TaxID currParentTaxId = node->parentTaxId;
        while (currParentTaxId != currTaxId) {
            if (ancTaxIdsCounts.find(currParentTaxId) != ancTaxIdsCounts.end()) {
                ancTaxIdsCounts[currParentTaxId].update(currWeight, currTaxId);
            } else {
                taxNode currParentNode;
                currParentNode.set(currWeight, false, currTaxId);
                ancTaxIdsCounts.insert(std::pair<TaxID,taxNode>(currParentTaxId, currParentNode));
            }
            // move up:
            currTaxId = currParentTaxId;
            node = taxonomy.taxonNode(currParentTaxId, false);
            currParentTaxId = node->parentTaxId;
        }
    }

    // select the lowest ancestor that meets the cutoff
    int minRank = INT_MAX;
    TaxID selctedTaxon = 0;

    for (std::map<TaxID,taxNode>::iterator it = ancTaxIdsCounts.begin(); it != ancTaxIdsCounts.end(); it++) {
        // consider only candidates:
        if (!(it->second.isCandidate)) {
            continue;
        }

        double currPercent = float(it->second.weight) / totalAssignedSeqsWeights;
        if (currPercent >= majorityCutoff) {
            // iterate all ancestors to find lineage min rank (the candidate is a descendant of a node with this rank)
            TaxID currTaxId = it->first;
            TaxonNode const * node = taxonomy.taxonNode(currTaxId, false);
            int currMinRank = INT_MAX;
            TaxID currParentTaxId = node->parentTaxId;
            while (currParentTaxId != currTaxId) {
                int currRankInd = NcbiTaxonomy::findRankIndex(node->rank);
                if ((currRankInd > 0) && (currRankInd < currMinRank)) {
                    currMinRank = currRankInd;
                    // the rank can only go up on the way to the root, so we can break
                    break;
                }
                // move up:
                currTaxId = currParentTaxId;
                node = taxonomy.taxonNode(currParentTaxId, false);
                currParentTaxId = node->parentTaxId;
            }

            if ((currMinRank < minRank) || ((currMinRank == minRank) && (currPercent > selectedPercent))) {
                selctedTaxon = it->first;
                minRank = currMinRank;
                selectedPercent = currPercent;
            }
        }
    }

    return selctedTaxon;
}

///It analyses the result of linear search.
void Classifier::analyseResult(NcbiTaxonomy & ncbiTaxonomy, vector<Sequence> & seqSegments){
    SORT_PARALLEL(matchedKmerList.begin(), matchedKmerList.end(), Classifier::compareForAnalyzing);
    size_t numOfMatches = matchedKmerList.size();
    int currentQuery;
    vector<MatchedKmer> matchesOfCurrentQuery;


    size_t i = 0;
    size_t queryOffset;
    size_t queryEnd;
    while(i < numOfMatches) {
        currentQuery = matchedKmerList[i].queryID;
        queryOffset = i;
        while((currentQuery ==  matchedKmerList[i].queryID) && (i < numOfMatches)){
            matchesOfCurrentQuery.push_back(matchedKmerList[i]);
            i++;
        }
        queryEnd = i - 1;
        cout<<"query num: "<<currentQuery<<endl;
        if(currentQuery == 12){
            cout<<"here"<<endl;
        }
        TaxID selectedLCA = chooseBestTaxon(ncbiTaxonomy, seqSegments[currentQuery].length, queryOffset, queryEnd);
        cout<<endl;
        matchesOfCurrentQuery.clear();

    }
}

///For a query read, assign the best Taxon, using k-mer matches
TaxID Classifier::chooseBestTaxon(NcbiTaxonomy & ncbiTaxonomy, const size_t & queryLen, const size_t & offset, const size_t & end){
//    for(size_t i = offset; i < end + 1; i++){
//        cout<<matchedKmerList[i].queryID<<" "<<matchedKmerList[i].taxID<<" "<<matchedKmerList[i].queryFrame<<" "<<matchedKmerList[i].queryPos<<" "<<int(matchedKmerList[i].hammingDistance)<<" "<<matchedKmerList[i].redundancy<<endl;
//    }

    vector<ConsecutiveMathces> coMatches;

    float coverageThr = 0.3;
    int conCnt = 0;
    uint32_t gapCnt = 0;
    uint32_t hammingSum = 0;
    uint32_t conBegin = 0;
    uint32_t conEnd = 0;
    size_t beginIdx = 0;
    size_t endIdx = 0;


    size_t i = offset;

    ///This routine is for getting consecutive matched k-mer
    ///gapThr decides the maximun gap
    int currentFrame;
    int gapThr = 0;
    while(i < end) {
        currentFrame = matchedKmerList[i].queryFrame;
        while ((matchedKmerList[i + 1].queryFrame == matchedKmerList[i].queryFrame) && (i < end)) {
            if (matchedKmerList[i + 1].queryPos <= matchedKmerList[i].queryPos + (gapThr + 1) * 3) {
                if (conCnt == 0) {
                    conBegin = matchedKmerList[i].queryPos;
                    beginIdx = i;
                }
                conCnt++;
                hammingSum += matchedKmerList[i].hammingDistance;
                if (matchedKmerList[i + 1].queryPos != matchedKmerList[i].queryPos) {
                    gapCnt += (matchedKmerList[i + 1].queryPos - matchedKmerList[i].queryPos) / 3 - 1;
                }
            } else {
                if (conCnt > 0) {
                    conCnt++;
                    hammingSum += matchedKmerList[i].hammingDistance;
                    conEnd = matchedKmerList[i].queryPos;
                    endIdx = i;
                    coMatches.emplace_back(conBegin, conEnd, hammingSum, gapCnt, beginIdx, endIdx);
                    cout << currentFrame << " " << conBegin << " " << conEnd << " " << conCnt << " " << gapCnt << " "
                         << hammingSum << endl;
                    conCnt = 0;
                    gapCnt = 0;
                    hammingSum = 0;
                }
            }
            i++;
        }

        if (conCnt > 0) {
            conCnt++;
            hammingSum += matchedKmerList[i].hammingDistance;
            conEnd = matchedKmerList[i].queryPos;
            endIdx = i;
            coMatches.emplace_back(conBegin, conEnd, hammingSum, gapCnt, beginIdx, endIdx);
            cout << currentFrame << " " << conBegin << " " << conEnd << " " << conCnt << " " << gapCnt << " "
                 << hammingSum << endl;
            conCnt = 0;
            gapCnt = 0;
            hammingSum = 0;
        }

        i++;
    }

    SORT_PARALLEL(coMatches.begin(), coMatches.end(), Classifier::compareConsecutiveMatches);


    ///Align consecutive matches back to query.
    vector<ConsecutiveMathces> alignedCoMatches;

    alignedCoMatches.push_back(coMatches[0]);
    auto alignedBegin = alignedCoMatches.begin();
    int isOverlaped= 0;
    int overlappedIdx = 0;
    for(size_t i2 = 1; i2 < coMatches.size(); i2++){
        isOverlaped = 0;
        overlappedIdx = 0;
        for(size_t j = 0; j < alignedCoMatches.size(); j++){
            if((alignedCoMatches[j].begin < coMatches[i2].end) && (alignedCoMatches[j].end > coMatches[i2].begin)){ ///TODO check this condition
                isOverlaped = 1;
                overlappedIdx = j;
                break;
            }
        }

        if(1 == isOverlaped){
            continue;
        } else{
            alignedCoMatches.push_back(coMatches[i2]);
        }
    }

    for(size_t cs = 0; cs < alignedCoMatches.size(); cs++ ){
        for(size_t k = alignedCoMatches[cs].beginIdx ; k < alignedCoMatches[cs].endIdx + 1; k++ ){
            cout<<matchedKmerList[k].queryID<<" "<<matchedKmerList[k].queryFrame<<" "<<matchedKmerList[k].queryPos<<" "<<matchedKmerList[k].taxID<<" "<<int(matchedKmerList[k].hammingDistance)<<" "<<matchedKmerList[k].redundancy<<" "<<matchedKmerList[k].targetID<<endl;
        }
        cout<<(alignedCoMatches[cs].end - alignedCoMatches[cs].begin)/3 + 1<<endl;
    }

    ///Check a query coverage
    int maxNum = queryLen / 3 - kmerLength + 1;
    int matchedNum = 0;
    int coveredLen = 0;
    float coverage;
    for(size_t cm = 0 ; cm < alignedCoMatches.size(); cm ++){
        matchedNum += (alignedCoMatches[cm].end - alignedCoMatches[cm].begin)/3 + 1;
        coveredLen += alignedCoMatches[cm].end - alignedCoMatches[cm].begin + 24;
    }
    coverage = float(matchedNum) / float(maxNum);
    cout<<"coverage: "<<coverage<<endl;

    ///No classification for low coverage
    if(coverage < coverageThr){
        return 0;
    }

    ///TODO: how about considering hamming distance here?
    ///Get a lowest common ancestor, and check whether strain taxIDs are existing
    vector<TaxID> taxIdList;
    TaxID temp;

    for(size_t cs = 0; cs < alignedCoMatches.size(); cs++ ){
        for(size_t k = alignedCoMatches[cs].beginIdx ; k < alignedCoMatches[cs].endIdx + 1; k++ ){
            temp = matchedKmerList[k].taxID;
            taxIdList.push_back(temp);
        }
    }


    size_t numAssignedSeqs = 0;
    size_t numUnassignedSeqs = 0;
    size_t numSeqsAgreeWithSelectedTaxon = 0;
    double selectedPercent = 0;

    TaxID selectedLCA = match2LCA(taxIdList, ncbiTaxonomy, 0.8, numAssignedSeqs,
                                  numUnassignedSeqs, numSeqsAgreeWithSelectedTaxon,
                                  selectedPercent);


    ///TODO optimize strain specific classification criteria
    ///Strain classification only for high coverage with LCA of species level
    int maxStrainCnt = 0;
    if(coverage > 0.90 && NcbiTaxonomy::findRankIndex(ncbiTaxonomy.taxonNode(selectedLCA)->rank) == 4){ /// There are more strain level classifications with lower coverage threshold, but also with more false postives. 0.8~0.85 looks good.
        int strainCheck = 0;
        unordered_map<TaxID, int> strainMatchCnt;
        TaxID strainTaxId;

        for(size_t cs = 0; cs < alignedCoMatches.size(); cs++ ){
            for(size_t k = alignedCoMatches[cs].beginIdx ; k < alignedCoMatches[cs].endIdx + 1; k++ ){
                temp = matchedKmerList[k].taxID;
                if(selectedLCA != temp && ncbiTaxonomy.IsAncestor(selectedLCA, temp)){
                    if(strainMatchCnt.find(temp) == strainMatchCnt.end()){
                        strainCheck ++;
                        strainTaxId = temp;
                        strainMatchCnt.insert(pair<TaxID, int>(temp, 1));
                    } else {
                        strainMatchCnt[temp] ++;
                    }
                }
            }
        }

        if(strainCheck == 1){
            ///strain classification
            selectedLCA = strainTaxId;
            cout<<"strain level classification here: "<<selectedLCA<<endl;
            return selectedLCA;
        }
    }



    if(NcbiTaxonomy::findRankIndex(ncbiTaxonomy.taxonNode(selectedLCA)->rank) == 3){
        cout<<"strain level classification: "<<selectedLCA<<endl;
    }else {
        cout<<selectedLCA<<" "<<selectedPercent<<endl;
    }

    return selectedLCA;
}

void Classifier::checkAndGive(vector<uint32_t> & posList, vector<uint8_t> & hammingList, const uint32_t & pos, const uint8_t & hammingDist){
    size_t vectorSize = posList.size();
    vector<uint32_t>::iterator it = posList.begin();
    vector<uint8_t>::iterator it2 = hammingList.begin();

    if(posList.empty()){
        return;
    }

    if(pos > posList.back()){
        posList.push_back(pos);
        hammingList.push_back(hammingDist);
        return;
    }
    for(uint32_t i = 0; i < vectorSize; i++){
        if(posList[i] == pos) {
            return;
        }
        if(posList[i] > pos){
            posList.insert(it + i, pos);
            hammingList.insert(it2 + i, hammingDist);
            return;
        }
    }
}

///It reads differential index and return "current + (next - current)", which is equal to next.
uint64_t Classifier::getNextTargetKmer(uint64_t lookingTarget, const uint16_t* targetDiffIdxList, size_t & diffIdxPos){
    uint16_t fragment;
    uint64_t diffIn64bit = 0;
    for(int i = 0; i < 5; i++){
        fragment = targetDiffIdxList[diffIdxPos];
        diffIdxPos++;
        if(fragment & (0x1u << 15))
        {
            fragment &= ~(1<<15u);
            diffIn64bit |= fragment;
            break;
        }
        diffIn64bit |= fragment;
        diffIn64bit <<= 15u;
    }
    return diffIn64bit + lookingTarget;
}

///
uint8_t Classifier::getHammingDistance(uint64_t kmer1, uint64_t kmer2) {
    uint8_t hammingDist = 0;
    for(int i = 0; i < 8 ; i++){
        hammingDist += hammingLookup[GET_3_BITS(kmer1)][GET_3_BITS(kmer2)];
        kmer1 >>= 3U;
        kmer2 >>= 3U;
    }
    return hammingDist;
}

void Classifier::writeResultFile(vector<MatchedKmer> & matchList, const char * queryFileName) {
    char suffixedResultFileName[1000];
    sprintf(suffixedResultFileName,"%s_result", queryFileName);
    numOfSplit++;
    sort(matchList.begin(), matchList.end(), [=](MatchedKmer x, MatchedKmer y) { return x.queryID < y.queryID; });
    cout<<suffixedResultFileName<<endl;
    FILE * fp = fopen(suffixedResultFileName,"wb");
    fwrite(&(matchList[0]), sizeof(MatchedKmer), matchList.size(), fp);
    fclose(fp);
    matchList.clear();
}

int Classifier::getNumOfSplits() const {
    return this->numOfSplit;
}

bool Classifier::compareForAnalyzing( const MatchedKmer & a, const MatchedKmer & b) {
    if (a.queryID < b.queryID) return true;
    else if (a.queryID == b.queryID) {
        if (a.queryFrame < b.queryFrame) return true;
        else if (a.queryFrame == b.queryFrame) {
            if (a.queryPos < b.queryPos) return true;
        }
    }
    return false;
}


bool Classifier::compareForLinearSearch(const QueryKmer & a, const QueryKmer & b){
    return a.ADkmer < b.ADkmer;
}

bool Classifier::compareConsecutiveMatches(const ConsecutiveMathces & a, const ConsecutiveMathces & b){
    if((a.end - a.begin) > (b.end- b.begin)){
        if((a.end - a.begin) == (b.end - b.begin + 1)){
            return (a.endIdx - a.beginIdx + 1) * 2 / ((a.hamming+1)*(a.gapCnt+1)) > (b.endIdx - b.beginIdx +1) * 2 / ((b.hamming + 1) * (b.gapCnt + 1));
        }
        return true;
    }else if((a.end - a.begin) == (b.end- b.begin)){
            return (a.endIdx - a.beginIdx + 1) * 2 / ((a.hamming+1)*(a.gapCnt+1)) > (b.endIdx - b.beginIdx +1) * 2 / ((b.hamming + 1) * (b.gapCnt + 1));
    }
    return false;
}

void Classifier::writeLinearSearchResult() {
//    char suffixedResultFileName[1000];
//    sprintf(suffixedResultFileName,"%s_linear", queryFileName);
//    FILE * fp = fopen(suffixedResultFileName,"w");
    size_t numOfMatches = matchedKmerList.size();
    for(size_t i = 0; i < numOfMatches; i++){
        cout << matchedKmerList[i].queryFrame << " " << matchedKmerList[i].queryID << " " << matchedKmerList[i].queryPos << " " << matchedKmerList[i].taxID << " " << int(matchedKmerList[i].hammingDistance) << endl;

    }
}