#include "RockSampleModel.hpp"

#include <cmath>                        // for pow, floor
#include <cstddef>                      // for size_t
#include <cstdlib>                      // for exit

#include <fstream>                      // for operator<<, basic_ostream, endl, basic_ostream<>::__ostream_type, ifstream, basic_ostream::operator<<, basic_istream, basic_istream<>::__istream_type
#include <iostream>                     // for cout, cerr
#include <memory>                       // for unique_ptr, default_delete
#include <random>                       // for uniform_int_distribution, bernoulli_distribution
#include <set>                          // for set, _Rb_tree_const_iterator, set<>::iterator
#include <string>                       // for string, getline, char_traits, basic_string
#include <tuple>                        // for tie, tuple
#include <unordered_map>                // for unordered_map<>::value_type, unordered_map
#include <utility>                      // for move, pair, make_pair
#include <vector>                       // for vector, vector<>::reference, __alloc_traits<>::value_type, operator==

#include <boost/program_options.hpp>    // for variables_map, variable_value

#include "defs.hpp"                     // for RandomGenerator, make_unique
#include "problems/GridPosition.hpp"    // for GridPosition, operator<<
#include "problems/ModelWithProgramOptions.hpp"  // for ModelWithProgramOptions
#include "solver/Action.hpp"            // for Action
#include "solver/ChangeType.hpp"        // for ChangeType
#include "solver/Model.hpp"             // for Model::StepResult, Model
#include "solver/Observation.hpp"       // for Observation
#include "solver/State.hpp"             // for State, operator<<, State::Hash, operator==

#include "RockSampleState.hpp"          // for RockSampleState

using std::cerr;
using std::cout;
using std::endl;
namespace po = boost::program_options;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
RockSampleModel::RockSampleModel(RandomGenerator *randGen,
        po::variables_map vm) : ModelWithProgramOptions(randGen, vm),
    goodRockReward(vm["problem.goodRockReward"].as<double>()),
    badRockPenalty(vm["problem.badRockPenalty"].as<double>()),
    exitReward(vm["problem.exitReward"].as<double>()),
    illegalMovePenalty(
            vm["problem.illegalMovePenalty"].as<double>()),
    halfEfficiencyDistance(
            vm["problem.halfEfficiencyDistance"].as<double>()) {
#pragma GCC diagnostic pop
    // Read the map from the file.
    std::ifstream inFile;
    char const *mapPath = vm["problem.mapPath"].as<std::string>().c_str();
    inFile.open(mapPath);
    if (!inFile.is_open()) {
        std::cerr << "Fail to open " << mapPath << "\n";
        exit(1);
    }
    inFile >> nRows >> nCols;
    std::string tmp;
    getline(inFile, tmp);
    for (long i = 0; i < nRows; i++) {
        getline(inFile, tmp);
        mapText.push_back(tmp);
    }
    inFile.close();

    initialise();
    cout << "Constructed the RockSampleModel" << endl;
    cout << "Discount: " << getDiscountFactor() << endl;
    cout << "Size: " << nRows << " by " << nCols << endl;
    cout << "Start: " << startPos.i << " " << startPos.j << endl;
    cout << "nRocks: " << nRocks << endl;
    cout << "Rock 0: " << rockPositions[0] << endl;
    cout << "Rock 1: " << rockPositions[1] << endl;
    cout << "good rock reward: " << goodRockReward << endl;
    cout << "nActions: " << nActions << endl;
    cout << "nObservations: " << nObservations << endl;
    cout << "nStVars: " << nStVars << endl;
    cout << "Random initial states:" << endl;
    cout << *sampleAnInitState() << endl;
    cout << *sampleAnInitState() << endl;
    cout << *sampleAnInitState() << endl;
    cout << *sampleAnInitState() << endl;

    cout << "nParticles: " << getNParticles() << endl;
    cout << "Environment:" << endl;
    drawEnv(cout);
}

void RockSampleModel::initialise() {
    nRocks = 0;
    GridPosition p;
    for (p.i = 0; p.i < nRows; p.i++) {
        envMap.emplace_back();
        for (p.j = 0; p.j < nCols; p.j++) {
            char c = mapText[p.i][p.j];
            CellType cellType;
            if (c == 'o') {
                rockPositions.push_back(p);
                cellType = (CellType)(ROCK + nRocks);
                nRocks++;
            } else if (c == 'G') {
                cellType = GOAL;
            } else if (c == 'S') {
                startPos = p;
                cellType = EMPTY;
            } else {
                cellType = EMPTY;
            }
            envMap.back().push_back(cellType);
        }
    }

    nActions = 5 + nRocks;
    nObservations = 2;
    nStVars = 2 + nRocks;
    minVal = -illegalMovePenalty / (1 - getDiscountFactor());
    maxVal = goodRockReward * nRocks + exitReward;
}

std::unique_ptr<State> RockSampleModel::sampleAnInitState() {
    return std::make_unique<RockSampleState>(startPos, sampleRocks());
}

std::unique_ptr<State> RockSampleModel::sampleStateUniform() {
    return std::make_unique<RockSampleState>(samplePosition(), sampleRocks());
}

GridPosition RockSampleModel::samplePosition() {
    long i = std::uniform_int_distribution<long>(0, nRows - 1)(*randGen);
    long j = std::uniform_int_distribution<long>(0, nCols - 1)(*randGen);
    return GridPosition(i, j);
}

std::vector<bool> RockSampleModel::sampleRocks() {
    return decodeRocks(std::uniform_int_distribution<long>
                (0, (1 << nRocks) - 1)(*randGen));
}

std::vector<bool> RockSampleModel::decodeRocks(long val) {
    std::vector<bool> isRockGood;
    for (int j = 0; j < nRocks; j++) {
        isRockGood.push_back(val &  (1 << j));
    }
    return isRockGood;
}

bool RockSampleModel::isTerm(State const &state) {
    RockSampleState const *rockSampleState =
        static_cast<RockSampleState const *>(&state);
    GridPosition pos = rockSampleState->getPosition();
    return envMap[pos.i][pos.j] == GOAL;
}

double RockSampleModel::solveHeuristic(State const &state) {
    RockSampleState const *rockSampleState =
        static_cast<RockSampleState const *>(&state);
    double qVal = 0;
    double currentDiscount = 1;
    GridPosition currentPos(rockSampleState->getPosition());
    std::vector<bool> rockStates(rockSampleState->getRockStates());

    std::set<int> goodRocks;
    for (int i = 0; i < nRocks; i++) {
        if (rockStates[i]) {
            goodRocks.insert(i);
        }
    }
    while (!goodRocks.empty()) {
        std::set<int>::iterator it = goodRocks.begin();
        int bestRock = *it;
        long lowestDist =
            rockPositions[bestRock].manhattanDistanceTo(currentPos);
        ++it;
        for (; it != goodRocks.end(); ++it) {
            long dist = rockPositions[*it].manhattanDistanceTo(currentPos);
            if (dist < lowestDist) {
                bestRock = *it;
                lowestDist = dist;
            }
        }
        currentDiscount *= std::pow(getDiscountFactor(), lowestDist);
        qVal += currentDiscount * goodRockReward;
        goodRocks.erase(bestRock);
        currentPos = rockPositions[bestRock];
    }
    currentDiscount *= std::pow(getDiscountFactor(), nCols - currentPos.j);
    qVal += currentDiscount * exitReward;
    // dispState(s, cerr);
    // cerr << endl << "Heuristic: " << *qVal << endl;
    return qVal;
}

double RockSampleModel::getDefaultVal() {
    return minVal;
}

std::pair<std::unique_ptr<RockSampleState>,
        bool> RockSampleModel::makeNextState(
        RockSampleState const &state, Action const &action) {
    GridPosition pos(state.getPosition());
    std::vector<bool> rockStates(state.getRockStates());
    bool isValid = true;
    if (action >= CHECK + nRocks) {
        cerr << "Invalid action: " << action << endl;
    } else if (action >= CHECK) {
        // Do nothing - the state remains the same.
    } else if (action == SAMPLE) {
        int rockNo = envMap[pos.i][pos.j] - ROCK;
        if (0 <= rockNo && rockNo < nRocks) {
            rockStates[rockNo] = false;
        } else {
            // cerr << "Cannot sample at " << pos << " - no rock!" << endl;
            isValid = false;
        }
    } else {
        if (action == NORTH) {
            pos.i -= 1;
        } else if (action == EAST) {
            pos.j += 1;
        } else if (action == SOUTH) {
            pos.i += 1;
        } else if (action == WEST) {
            pos.j -= 1;
        } else {
            cerr << "Invalid action: " << action << endl;
        }
        // If the position is now invalid, reset it.
        if (pos.i < 0 || pos.i >= nRows || pos.j < 0 || pos.j >= nCols) {
            pos = state.getPosition();
            isValid = false;
        }
    }
    return std::make_pair(std::make_unique<RockSampleState>(pos, rockStates),
            isValid);
}

RockSampleModel::RSObservation RockSampleModel::makeObs(Action const &action,
        RockSampleState const &nextState) {
    if (action < CHECK) {
        return RSObservation::NONE;
    }
    int rockNo = action - CHECK;
    GridPosition pos(nextState.getPosition());
    std::vector<bool> rockStates(nextState.getRockStates());
    double dist = pos.euclideanDistanceTo(rockPositions[rockNo]);
    double efficiency = (1 + std::pow(2, -dist / halfEfficiencyDistance)) * 0.5;
    // cerr << "D: " << dist << " E:" << efficiency << endl;
    if (std::bernoulli_distribution(efficiency)(*randGen)) {
        return rockStates[rockNo] ? RSObservation::GOOD : RSObservation::BAD; // Correct obs.
    } else {
        return rockStates[rockNo] ? RSObservation::BAD : RSObservation::GOOD; // Incorrect obs.
    }
}

Model::StepResult RockSampleModel::generateStep(State const &state,
        Action const &action) {
    RockSampleState const *rockSampleState =
        static_cast<RockSampleState const *>(&state);
    Model::StepResult result;
    result.action = action;

    std::unique_ptr<RockSampleState> nextState = makeNextState(
                *rockSampleState, action).first;

    result.observation.push_back((double)makeObs(action, *nextState));
    result.immediateReward = getReward(state, action);
    result.isTerminal = isTerm(*nextState);
    result.nextState = std::move(nextState);
    return result;
}

double RockSampleModel::getReward(State const & /*state*/) {
    return 0;
}

double RockSampleModel::getReward(State const &state, Action const &action) {
    RockSampleState const *rockSampleState =
        static_cast<RockSampleState const *>(&state);

    std::unique_ptr<RockSampleState> nextState;
    bool isLegal;
    std::tie(nextState, isLegal) = makeNextState(*rockSampleState, action);

    if (!isLegal) {
        return -illegalMovePenalty;
    }
    if (isTerm(*nextState)) {
        return exitReward;
    }

    if (action == SAMPLE) {
        GridPosition pos = rockSampleState->getPosition();
        int rockNo = envMap[pos.i][pos.j] - ROCK;
        if (0 <= rockNo && rockNo < nRocks) {
            return rockSampleState->getRockStates()[rockNo] ? goodRockReward
                   : -badRockPenalty;
        } else {
            cerr << "Invalid sample action!?!" << endl;
            return -illegalMovePenalty;
        }
    }
    return 0;
}

std::vector<std::unique_ptr<State>> RockSampleModel::generateParticles(
        Action const &action, Observation const &obs,
        std::vector<State *> const &previousParticles) {
    std::vector<std::unique_ptr<State>> newParticles;
    // If it's a CHECK action, we condition on the observation.
    if (action >= CHECK) {
        int rockNo = action - CHECK;
        typedef std::unordered_map<RockSampleState, double,
                State::Hash> WeightMap;
        WeightMap weights;
        double weightTotal = 0;
        for (State *state : previousParticles) {
            RockSampleState const *rockSampleState =
                static_cast<RockSampleState const *>(state);
            GridPosition pos(rockSampleState->getPosition());
            double dist = pos.euclideanDistanceTo(rockPositions[rockNo]);
            double efficiency = ((1
                                  + std::pow(2, -dist
                                          / halfEfficiencyDistance)) * 0.5);
            bool rockIsGood = rockSampleState->getRockStates()[rockNo];
            double probability;
            if ((rockIsGood && obs[0] == (double)RSObservation::GOOD)
                || (!rockIsGood && obs[0] == (double)RSObservation::BAD)) {
                probability = efficiency;
            } else {
                probability = 1 - efficiency;
            }
            weights[*rockSampleState] += probability;
            weightTotal += probability;
        }
        double scale = getNParticles() / weightTotal;
        for (WeightMap::value_type &it : weights) {
            double proportion = it.second * scale;
            int numToAdd = std::floor(proportion);
            if (std::bernoulli_distribution(proportion - numToAdd)(*randGen)) {
                numToAdd += 1;
            }
            for (int i = 0; i < numToAdd; i++) {
                newParticles.push_back(std::make_unique<RockSampleState>(it.
                                first));
            }
        }

    } else {
        // It's not a CHECK action, so we just add each resultant state.
        for (State *state : previousParticles) {
            RockSampleState const *rockSampleState =
                static_cast<RockSampleState const *>(state);
            newParticles.push_back(makeNextState(*rockSampleState,
                            action).first);
        }
    }
    return newParticles;
}

std::vector<std::unique_ptr<State>> RockSampleModel::generateParticles(
        Action const &action, Observation const &obs) {
    std::vector<std::unique_ptr<State>> particles;
    while (particles.size() < getNParticles()) {
        std::unique_ptr<State> state = sampleStateUniform();
        Model::StepResult result = generateStep(*state, action);
        if (obs == result.observation) {
            particles.push_back(std::move(result.nextState));
        }
    }
    return particles;
}

std::vector<long> RockSampleModel::loadChanges(char const */*changeFilename*/) {
    std::vector<long> result;
    return result;
}

void RockSampleModel::update(long /*time*/,
        std::vector<std::unique_ptr<State>> */*affectedRange*/,
        std::vector<ChangeType> */*typeOfChanges*/) {
}

bool RockSampleModel::modifStSeq(std::vector<State const *> const & /*states*/,
        long /*startAffectedIdx*/, long /*endAffectedIdx*/,
        std::vector<std::unique_ptr<State>> */*modifStSeq*/,
        std::vector<Action> */*modifActSeq*/,
        std::vector<Observation> */*modifObsSeq*/,
        std::vector<double> */*modifRewSeq*/) {
    return false;
}

void RockSampleModel::dispAct(Action const &action, std::ostream &os) {
    if (action >= CHECK) {
        os << "CHECK-" << action - CHECK;
        return;
    }
    switch (action) {
    case NORTH:
        os << "NORTH";
        break;
    case EAST:
        os << "EAST";
        break;
    case SOUTH:
        os << "SOUTH";
        break;
    case WEST:
        os << "WEST";
        break;
    case SAMPLE:
        os << "SAMPLE";
        break;
    default:
        os << "ERROR-" << action;
        break;
    }
}

void RockSampleModel::dispCell(CellType cellType, std::ostream &os) {
    if (cellType >= ROCK) {
        os << std::hex << cellType - ROCK;
        os << std::dec;
        return;
    }
    switch (cellType) {
    case EMPTY:
        os << '.';
        break;
    case GOAL:
        os << 'G';
        break;
    default:
        os << "ERROR-" << cellType;
        break;
    }
}

void RockSampleModel::dispObs(Observation const &obs, std::ostream &os) {
    switch ((int)obs[0]) {
    case (int)RSObservation::NONE:
        os << "NONE";
        break;
    case (int)RSObservation::GOOD:
        os << "GOOD";
        break;
    case (int)RSObservation::BAD:
        os << "BAD";
        break;
    default:
        os << "ERROR-" << obs[0];
        break;
    }
}

void RockSampleModel::drawEnv(std::ostream &os) {
    for (std::vector<CellType> &row : envMap) {
        for (CellType cellValue : row) {
            dispCell(cellValue, os);
        }
        os << endl;
    }
}

void RockSampleModel::drawState(State const &state, std::ostream &os) {
    RockSampleState const *rockSampleState =
        static_cast<RockSampleState const *>(&state);
    os << state << endl;
    GridPosition pos(rockSampleState->getPosition());
    for (std::size_t i = 0; i < envMap.size(); i++) {
        for (std::size_t j = 0; j < envMap[0].size(); j++) {
            if ((long)i == pos.i && (long)j == pos.j) {
                os << "x";
                continue;
            }
            dispCell(envMap[i][j], os);
        }
        os << endl;
    }
}
