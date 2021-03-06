/*
 * BDDTab - A BDD-based tableau reasoner for propositional modal logic
 * by Kerry Olesen
 * 2014
 */

#include "bddtab.h"


// ----------------------- Global variable declarations --------------------- //
// Correspondence between KFormulae and BDD variables:
bool(*compareptr)(const KFormula*, const KFormula*) = kformulaComp;
std::map<const KFormula*, int, bool(*)(const KFormula*,const KFormula*)> atomsToVars (compareptr);
std::vector<const KFormula*> varsToAtoms(1);

// Correspondence between modal BDD variables (/formulae) and their 'children' variables (/formulae).
std::vector<std::unordered_set<int>> varsToChildren(1);

// Cache of unboxings and undiamondings:
std::vector<bdd> unboxings(1);
std::vector<bool> unboxed(1);
std::vector<bdd> undiamondings(1);
std::vector<bool> undiamonded(1);
// Note: as bdd variables are integers in a fixed range, vectors are the most
// efficient standard containers for mapping from variables to other things.
// (In terms of time, that is)

// BDD Variable to indicate whether any <>'s are present.
// Necessary for some optimisations for K, not used for S4.
const int existsDia = 0;

// Number of BDD variables.
int numVars = 1;

// Number of different roles present in the input formulae.
int numRoles = 0;
bool inverseRoles = false;

// Repeated node checking. Results caching:
std::unordered_set<bdd, BddHasher> satCache;
std::deque<bdd> satCacheDeque;
std::map<std::vector<int>, bdd> unsatCache;
std::deque<std::vector<int>> unsatCacheDeque;
std::unordered_map<bdd, std::unordered_set<int>, BddHasher> saturationUnsatCache;
std::deque<bdd> saturationUnsatCacheDeque;
size_t maxCacheSize = 8000;

// Loop checking / cyclic dependencies:
// All previous worlds on the current branch of the tableau.
std::unordered_set<bdd, BddHasher> dependentBDDs;
// All worlds that are currently undecided, but have been assumed true at some point.
std::unordered_set<bdd, BddHasher> everAssumedSatBDDs;
// Temporary cache of sat results made while under certain assumptions.
std::list<std::pair<std::unordered_set<bdd, BddHasher>, bdd>> tempSatCaches;


// Global assumptions:
bool globalAssumptions = false;
bdd gammaBDD;
std::unordered_set<int> gammaChildren;

// S4 flag
bool S4 = false;

// BDD style unsat cache
bool bddUnsatCache = false;
bdd unsatCacheBDD;
// Saturation phase unsat cache
bool useSaturationUnsatCache = false;

// Whether to use an unsat cache at all.
bool useUnsatCache = true;

// Expore satisfying valuations from right-to-left.
// (Default is left-to-right)
bool rightToLeft = false;

// Enable dynamic BDD variable reordering
bool reorder = false;

// If reordering is enabled, only do so while constructing Gamma.
bool onlyGamma = false;

// Use BDDs to completely normalise all formulae as a preprocessing step.
bool bddNormalise = false;

// Do an ontology classification instead of a single provability task.
bool classify = false;


// Algorithm statistics:
bool verbose = false;
bool periodicSummary = false;// Output summaries periodically during execution.
int period = 1;// Number of modal jumps between periodic summaries.
size_t depth = 0;	// Modal depth of the explored tableau.
size_t maxDepth = 0;
int totalModalJumpsExplored = 0;
int totalBDDRefinements = 0;
int totalSatisfiableModalJumps = 0;

int cachedUnboxings = 0;
int unboxCacheHits = 0;
int cachedUndiamondings = 0;
int undiamondCacheHits = 0;

int satCacheAdds = 0;//   Number of sat results that were cached.
int unsatCacheAdds = 0;// Number of unsat results that were cached.
int satCacheHits = 0;
int unsatCacheHits = 0;

int numFalseFromBox = 0;// Number of unboxings that were immediately unsatisfiable.
int numFalseFromDia = 0;// Number of modal jumps that were immediately unsatisfiable.
int numFalseFromRef = 0;// Number of refinements that were immediately unsatisfiable.

int numResVarsIgnoredFromBox = 0;// Modal variables ignored in unboxing base case.
int numResVarsIgnoredFromDia = 0;// Modal variables ignored in modal jump base case.
int numResVarsIgnoredFromGeneral = 0;// Modal variables ignored in general case.

int loopsDetected = 0;
int numTempSatCaches = 0;// Number of results cached while under assumptions
int tempSatCachesConfirmed = 0;// Results whose assumptions were confirmed.
int tempSatCachesRejected = 0;// Results whose assumptions were rejected.

int numVarsReduced = 0;// BoxVars determined semantically equivalent through bdd normalisation.


// --------------------- Function implementations --------------------------- //

/*
 * BddReasoner will read in a formula 'psi' from standard in,
 * and will output whether psi is provable or not.
 * It will decide this by applying the tableau method,
 * with Binary Decision Diagrams (BDDs) as the base data structure.
 */
int main(int argc, char * argv[]) {
	
	processArgs(argc, argv);
	
	// TODO: I really doubt this is portable.
	// Messing with stack size limits. 'Cause 64 bit system is ruining my recursive depth.
	const rlim_t desiredStackSize = 32 * 1024 * 1024;   // min stack size = 32 MB
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
    	if (rl.rlim_cur < desiredStackSize) {
			rl.rlim_cur = desiredStackSize;
			if (setrlimit(RLIMIT_STACK, &rl) != 0) {
				std::cerr << "Could not increase stack size." << std::endl;
			}
		}
	}
	
	if (classify) {
		// Instead of performing a single provability/satisfiablity task,
		// treat the input as an ontology definition, and perform a classification
		// of all atomic propositions.
		performClassification();
		return 0;
	}
	
	// Parse the formula psi, negate it, and translate to BoxNNF.
	// Further normalise by canonically ordering subformulae. NOTE: replaced by BDD normalising.
	// (see toBoxNNF() for details on BoxNNF)
	KFormula* notpsiNNF;
	// Read in the formula from standard in.
	std::string s;
	std::getline(std::cin, s);
	if (s.length() == 0 && !globalAssumptions) {
		std::cout << "Empty formula is provable." << std::endl;
		exit(1);
	} else if (s.length() == 0) {
		notpsiNNF = new KFormula(true);
	} else {
		notpsiNNF = toBoxNNF(new KFormula(KFormula::NOT,
												KFormula::parseKFormula(s.c_str()),
												NULL));
	}
	 
	
	KFormula* gammaNNF;
	if (globalAssumptions) {
		// Parse global assumptions from input.
		std::getline(std::cin, s);
		if (s.length() != 0) {
			gammaNNF = toBoxNNF(KFormula::parseKFormula(s.c_str()));
		} else {
			// Have vacuous global assumptions.
			gammaNNF = toBoxNNF(new KFormula(true));
		}
	} else {
		// Have vacuous global assumptions.
		gammaNNF = toBoxNNF(new KFormula(true));
	}
	
	// Set an integer for each role.
	std::unordered_set<std::string> roles;
	findAllRoles(notpsiNNF, roles);
	findAllRoles(gammaNNF, roles);
	std::unordered_map<std::string, int> roleMap;
	assignRoleInts(roles, roleMap);
	applyRoleInts(notpsiNNF, roleMap);
	applyRoleInts(gammaNNF, roleMap);
	
	// Initialise the bdd framework.
	bdd_init(10000, 1000);
	bdd_setcacheratio(8);
	bdd_setmaxincrease(2500000);// 50 Mb
	
	unsatCacheBDD = bddtrue;
	
	// Extract the set of 'atomic' formulae, and relate each atom to a BDD variable.
	// In this case all []phi are atomic, as well as all atomic propositions.
	std::vector<KFormula*> atoms;
	std::deque<KFormula*> formulae;
	formulae.push_back(gammaNNF);
	formulae.push_back(notpsiNNF);
	relateAtomsAndBDDVars(atoms, formulae);
	
	// Dynamic variable reordering. Super naive, but often effective.
	if (reorder) {
		bdd_varblockall();
		bdd_autoreorder(BDD_REORDER_WIN2ITE);
	}
	
	
	// Build and store the BDD of gamma
	gammaBDD = toBDD(gammaNNF);
	computeChildren(gammaNNF, gammaChildren);
	
	if (onlyGamma) {
		bdd_clrvarblocks();
		bdd_disable_reorder();
	}
	
	// Build a BDD of notpsi and gamma
	bdd notpsiAndGammaBDD = toBDD(notpsiNNF) & gammaBDD;
	
	
	bool isSat = isSatisfiable(notpsiAndGammaBDD);
	if (S4) {
		std::cout << "S4:";
	} else if (inverseRoles) {
		// Not implemented
	} else {
		std::cout << "K:";
	}

	
	if (isSat) {
		std::cout << "Psi is Not provable from Gamma [Not-psi and Gamma is Satisfiable].";
	} else {
		std::cout << "Psi is Provable from Gamma [Not-psi and Gamma is Unsatisfiable].";
	}
	
	if (verbose) {
		printSummaryStatistics();
	} else {
		std::cout << std::endl;
	}
	
	bdd_done();
	
	return 0;
}

/*
 * 
 * 
 */
void performClassification() {

	// Read in the ontology as a modal formula from standard in.
	std::string s;
	std::getline(std::cin, s);
	if (s.length() == 0) {
		std::cout << "Nothing to do for empty ontology." << std::endl;
		exit(1);
	}
	
	KFormula* gammaNNF = toBoxNNF(KFormula::parseKFormula(s.c_str()));
	
	// Set an integer for each role.
	std::unordered_set<std::string> roles;
	findAllRoles(gammaNNF, roles);
	std::unordered_map<std::string, int> roleMap;
	assignRoleInts(roles, roleMap);
	applyRoleInts(gammaNNF, roleMap);
	
	// Initialise the bdd framework.
	bdd_init(10000, 1000);
	bdd_setcacheratio(8);
	bdd_setmaxincrease(2500000);// 50 Mb
	
	unsatCacheBDD = bddtrue;
	
	// Extract the set of 'atomic' formulae, and relate each atom to a BDD variable.
	// In this case all []phi are atomic, as well as all atomic propositions.
	std::vector<KFormula*> atoms;
	std::deque<KFormula*> formulae;
	formulae.push_back(gammaNNF);
	relateAtomsAndBDDVars(atoms, formulae);
	
	// Dynamic variable reordering. Super naive, but often effective.
	if (reorder) {
		bdd_varblockall();
		bdd_autoreorder(BDD_REORDER_WIN2ITE);
	}

	// Build and store the BDD of gamma
	gammaBDD = toBDD(gammaNNF);
	computeChildren(gammaNNF, gammaChildren);
	
	if (onlyGamma) {
		bdd_clrvarblocks();
		bdd_disable_reorder();
	}
	
	// Testing out removing reordering after constructing gamma.
	// Really, as a tableau method, there's often not much point in dynamic reordering
	// throughout the process. but since we're starting with gamma many many times,
	// Reordering for it makes sense.
	bdd_clrvarblocks();
	bdd_disable_reorder();
	
	// Find all the atomic proposition variables.
	std::vector<int> classes;
	for (int var = 1; var < numVars; ++var) {
		if (varsToAtoms.at(var)->getop() == KFormula::AP) {
			classes.push_back(var);
		}
	}
	
	// Test for satisfiability of the ontology:
	if (!isSatisfiable(gammaBDD)) {
		std::cout << "Ontology is unsatisfiable!" << std::endl;
		std::cout << "No more tests performed." << std::endl;
		exit(1);
	}
	
	// Test for satisfiability of each class:
	for (std::vector<int>::iterator classIt = classes.begin();
			classIt != classes.end(); ++classIt ) {
		if (!isSatisfiable(gammaBDD & bdd_ithvar(*classIt))) {
			std::cout << varsToAtoms.at(*classIt) << " is an empty class!" << std::endl;
		}
	}
	
	// Naively test for subsumption between every pair of classes:
	for (std::vector<int>::iterator leftIt = classes.begin(); leftIt != classes.end(); ++leftIt) {
		for (std::vector<int>::iterator rightIt = classes.begin(); rightIt != classes.end(); ++rightIt) {
			if (*leftIt != *rightIt) {
				if (!isSatisfiable(gammaBDD & (bdd_ithvar(*leftIt) & bdd_nithvar(*rightIt)))) {
					std::cout << *varsToAtoms.at(*leftIt) << " [= " << *varsToAtoms.at(*rightIt) << std::endl;
				}
			}
		}
	}
	
	if (verbose) {
		printSummaryStatistics();
	}
}

void processArgs(int argc, char * argv[]) {
	if (argc > 9) {
		printUsage();
		exit(1);
	}
	
	for (int i = 1; i < argc; ++i) {
		if (strncmp(argv[i], "-g", 2) == 0) {
			globalAssumptions = true;
		} else if (strncmp(argv[i], "-s4", 3) == 0) {
			S4 = true;
		} else if (strncmp(argv[i], "-v", 2) == 0) {
			verbose = true;
		} else if (strncmp(argv[i], "-buc", 4) == 0) {
			bddUnsatCache = true;
		} else if (strncmp(argv[i], "-nuc", 4) == 0) {
			useUnsatCache = false;
		} else if (strncmp(argv[i], "-suc", 4) == 0) {
			useSaturationUnsatCache = true;
		} else if (strncmp(argv[i], "-rtol", 5) == 0) {
			rightToLeft = true;
		} else if (strncmp(argv[i], "-reorder", 8) == 0) {
			reorder = true;
		} else if (strncmp(argv[i], "-onlygamma", 10) == 0) {
			onlyGamma = true;
		} else if (strncmp(argv[i], "-norm", 5) == 0) {
			bddNormalise = true;
		} else if (strncmp(argv[i], "-classify", 9) == 0) {
			classify = true;
		} else {
			printUsage();
			exit(1);
		}
	}
}

void printUsage() {
	std::cout << "Usage: bddReasoner [options]" << std::endl;
	std::cout <<
	"  -g		Read a second line of input as global assumptions."
	<< std::endl;
	std::cout <<
	"  -v		Output verbose summary statistics."
	<< std::endl;
	std::cout <<
	"  -s4		Treat R as transitive and reflexive."
	<< std::endl;
	std::cout <<
	"  -buc		Use a single bdd for the Unsat cache."
	<< std::endl;
	std::cout <<
	"  -nuc		Don't use an Unsat cache of any sort."
	<< std::endl;
	std::cout <<
	"  -suc		Do saturation unsat caching, instead of leaf caching."
	<< std::endl;
	std::cout <<
	"  -rtol		Explore saturated tableaux from right to left."
	<< std::endl;
	std::cout <<
	"  -reorder		Use dynamic BDD variable reordering."
	<< std::endl;
	std::cout <<
	"  -norm		Use BDDs to completely normalise formulae as a preprocessing step."
	<< std::endl;
	std::cout <<
	"  -classify		Perform a classification of all atomic formulae."
	<< std::endl;
}

void printSummaryStatistics() {
	// Algorithm Statistics:
	std::cout << " (" << satCacheAdds << ":" << satCacheHits;
	std::cout << " / " << unsatCacheAdds << ":" << unsatCacheHits << ")";
	std::cout << " [V: " << numVars << " - " << numVarsReduced << ",";
	std::cout << " D: " << depth << "/" << maxDepth << ",";
	std::cout << " MJ: " << totalModalJumpsExplored << ",";
	std::cout << " SatMJ: " << totalSatisfiableModalJumps << ",";
	std::cout << " UnsatMJ: " << totalModalJumpsExplored - totalSatisfiableModalJumps - loopsDetected << ",";
	std::cout << " Ref: " << totalBDDRefinements << ",";
	std::cout << " FF[]: " << numFalseFromBox << ",";
	std::cout << " FF<>: " << numFalseFromDia << ",";
	std::cout << " FFRef: " << numFalseFromRef << ",";
	if (!S4) {
		std::cout << " Ig[]: " << numResVarsIgnoredFromBox << ",";
		std::cout << " Ig<>: " << numResVarsIgnoredFromDia << ",";
		std::cout << " IgGen: " << numResVarsIgnoredFromGeneral << ",";
	}
	std::cout << " Loops: " << loopsDetected << ",";
	std::cout << " #Tmp: " << numTempSatCaches << ",";
	std::cout << " Rej: " << tempSatCachesRejected << ",";
	std::cout << " Conf: " << tempSatCachesConfirmed << ",";
	std::cout << " Ub+: " << cachedUnboxings << ",";
	std::cout << " UbHits: " << unboxCacheHits << ",";
	std::cout << " Ud+: " << cachedUndiamondings << ",";
	std::cout << " UdHits: " << undiamondCacheHits << "]";
	std::cout << std::endl;
}

/*
 *  BoxNNF: NNF with the following exceptions:
 *  BoxNNF(~[]phi) = ~BoxNNF([]phi)
 *  BoxNNF(<>phi) = ~[]BoxNNF(~phi)
 *	This is to normalise the modalities so that we don't get separate BDD 
 *	variables for <>~phi and []phi. These should share the same variable,
 *	as ~<>~phi = []phi.
 */
KFormula* toBoxNNF(KFormula* f) {
	switch (f->getop()) {
		case KFormula::AP:
			return f;
		case KFormula::AND:
			return new KFormula(KFormula::AND, toBoxNNF(&(f->getleft())), toBoxNNF(&(f->getright())));
		case KFormula::OR:
			return new KFormula(KFormula::OR, toBoxNNF(&(f->getleft())), toBoxNNF(&(f->getright())));
		case KFormula::BOX:
			{KFormula* tmpf = new KFormula(KFormula::BOX, toBoxNNF(&(f->getleft())), NULL);
			tmpf->setprop(f->getprop());
			return tmpf;}
		case KFormula::DIA:// BoxNNF(<>phi) = ~[]BoxNNF(~phi)
			{KFormula* tmpf = new KFormula(KFormula::BOX,
													toBoxNNF(new KFormula(KFormula::NOT,
														&(f->getleft()),
														NULL)),
													NULL);
			tmpf->setprop(f->getprop());// TODO this multimodal hack is really ugly...
			return new KFormula(KFormula::NOT,
								tmpf,
								NULL);}
		case KFormula::TRUE:
			return f;
		case KFormula::FALSE:
			return f;
		case KFormula::IMP:
			return toBoxNNF(new KFormula(KFormula::OR, new KFormula(KFormula::NOT, &(f->getleft()), NULL), &(f->getright())));
		case KFormula::NOT:
			switch (f->getleft().getop()) {
				case KFormula::AP:
					return f;
				case KFormula::AND:
					return new KFormula(KFormula::OR,
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getleft()), NULL)),
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getright()), NULL)));
				case KFormula::OR:
					return new KFormula(KFormula::AND,
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getleft()), NULL)),
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getright()), NULL)));
				case KFormula::BOX:// BoxNNF(~[]phi) = ~[]BoxNNF(phi)
					{KFormula* tmpf = new KFormula(KFormula::BOX,
											toBoxNNF(&(f->getleft().getleft())), NULL);
					tmpf->setprop(f->getleft().getprop());
					return new KFormula(KFormula::NOT,
										tmpf,
										NULL);}
				case KFormula::DIA:
					{KFormula* tmpf = new KFormula(KFormula::BOX,
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getleft()), NULL)),
								NULL);
					tmpf->setprop(f->getleft().getprop());
					return tmpf;}
				case KFormula::TRUE:
					return new KFormula(false);
				case KFormula::FALSE:
					return new KFormula(true);
				case KFormula::IMP:
					return toBoxNNF(new KFormula(KFormula::AND, &(f->getleft().getleft()), new KFormula(KFormula::NOT, &(f->getleft().getright()), NULL)));
				case KFormula::NOT:
					return toBoxNNF(&(f->getleft().getleft()));
				case KFormula::EQU:
					return new KFormula(KFormula::AND,
								new KFormula(KFormula::OR,
									toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getleft()), NULL)),
									toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft().getright()), NULL))),
								new KFormula(KFormula::OR,
									toBoxNNF(&(f->getleft().getleft())),
									toBoxNNF(&(f->getleft().getright()))));
				default:
					assert(false && "Defaulted out of complete switch.");
			}
		case KFormula::EQU:
			return new KFormula(KFormula::OR,
							new KFormula(KFormula::AND,
								toBoxNNF(&(f->getleft())),
								toBoxNNF(&(f->getright()))),
							new KFormula(KFormula::AND,
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getleft()), NULL)),
								toBoxNNF(new KFormula(KFormula::NOT, &(f->getright()), NULL))));
		default:
			assert(false && "Defaulted out of complete switch.");
	}
}

/*
 * Assumes BoxNNF
 */
void findAllRoles(KFormula* f, std::unordered_set<std::string>& roles) {
	switch (f->getop()) {
		case KFormula::AP:
			return;
		case KFormula::AND:
			findAllRoles(&(f->getleft()), roles);
			findAllRoles(&(f->getright()), roles);
			return;
		case KFormula::OR:
			findAllRoles(&(f->getleft()), roles);
			findAllRoles(&(f->getright()), roles);
			return;
		case KFormula::BOX:
			roles.insert(f->getprop());
			findAllRoles(&(f->getleft()), roles);
			return;
		case KFormula::NOT:
			findAllRoles(&(f->getleft()), roles);
			return;
		case KFormula::TRUE:
			return;
		case KFormula::FALSE:
			return;
		case KFormula::DIA:
			assert(false && "Role finding not defined for <>.");
		case KFormula::IMP:
			assert(false && "Role finding not defined for =>.");
		case KFormula::EQU:
			assert(false && "Role finding not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/*
 * Create a mapping from string roles to integer roles.
 */
void assignRoleInts(std::unordered_set<std::string>& roles,
		std::unordered_map<std::string, int>& roleMap) {
	// numRoles needs to be a global thing.
	// inverseRolesExist should be a global thing too.
	// Go through the roles
	for (std::unordered_set<std::string>::iterator roleIt = roles.begin();
			roleIt != roles.end(); ++roleIt) {
		if (roleIt->compare("") == 0) {
			// Empty string has no inverse role, give it a new positive role.
			numRoles++;
			roleMap.insert(std::pair<std::string, int>(*roleIt, numRoles));
			continue;
		}
		if (roleIt->at(0) != '-') {
			// Look for "-'thisRole'" in the roles we've already done
			if (roleMap.count("-" + *roleIt) != 0) {
				// Then this role gets minus the other role.
				roleMap.insert(std::pair<std::string, int>(*roleIt, - roleMap.at("-" + *roleIt)));
				inverseRoles = true;
				continue;
			}
		} else {
			// Look for 'thisRole' without the preceeding '-' in the ones we've already done.
			if (roleMap.count(roleIt->substr(1, std::string::npos)) != 0) {
				// Then this role gets minus the other role.
				roleMap.insert(std::pair<std::string, int>(
						*roleIt, - roleMap.at(roleIt->substr(1, std::string::npos))));
				inverseRoles = true;
				continue;
			}
		}
		// Otherwise, make a new positive role
		numRoles++;
		roleMap.insert(std::pair<std::string, int>(*roleIt, numRoles));
	}
}

/*
 * Traverse the formula, replacing string roles with integer roles.
 */
void applyRoleInts(KFormula* f, std::unordered_map<std::string, int>& roleMap) {
	switch (f->getop()) {
		case KFormula::AP:
			return;
		case KFormula::AND:
			applyRoleInts(&(f->getleft()), roleMap);
			applyRoleInts(&(f->getright()), roleMap);
			return;
		case KFormula::OR:
			applyRoleInts(&(f->getleft()), roleMap);
			applyRoleInts(&(f->getright()), roleMap);
			return;
		case KFormula::BOX:
			f->setrole(roleMap.at(f->getprop()));
			applyRoleInts(&(f->getleft()), roleMap);
			return;
		case KFormula::NOT:
			applyRoleInts(&(f->getleft()), roleMap);
			return;
		case KFormula::TRUE:
			return;
		case KFormula::FALSE:
			return;
		case KFormula::DIA:
			assert(false && "Role replacing not defined for <>.");
		case KFormula::IMP:
			assert(false && "Role replacing not defined for =>.");
		case KFormula::EQU:
			assert(false && "Role replacing not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/* 
 * Comparator function to use c++ sets of KFormula*
 */
bool kformulaComp(const KFormula* f1, const KFormula* f2) {
	return ((*f1).compare((*f2)) < 0);
}

/*
 *	Creates a mapping from BDD variables to formulae.
 *	The reverse mapping from formulae to BDD variables is written on the given
 *	formula tree directly.
 *
 *	Assumes given formulae are in BoxNNF.
 *
 *	The initial ordering of the BDD variables is breadth-first order,
 *	as the corresponding formulae are found in the original formula.
 */
void relateAtomsAndBDDVars(std::vector<KFormula*>& atoms, std::deque<KFormula*>& formulae) {
	while (!formulae.empty()) {
		KFormula* formula = formulae.front();
		formulae.pop_front();
		switch (formula->getop()) {
			case KFormula::AP:
				atoms.push_back(formula);
				break;
			case KFormula::BOX:
				atoms.push_back(formula);
				formulae.push_back(&(formula->getleft()));
				break;
			case KFormula::NOT:
				// Immediately continue with the unnegated formula.
				formulae.push_front(&(formula->getleft()));
				break;
			case KFormula::AND:// Fall through
			case KFormula::OR:
				// Breadth-first add from both sides.
				formulae.push_back(&(formula->getleft()));
				formulae.push_back(&(formula->getright()));
				break;
			case KFormula::TRUE:
				break;
			case KFormula::FALSE:
				break;
			case KFormula::DIA:
				assert(false && "Atom adding not defined for <>.");
			case KFormula::IMP:
				assert(false && "Atom adding not defined for =>.");
			case KFormula::EQU:
				assert(false && "Atom adding not defined for <=>.");
			default:
				assert(false && "Defaulted out of complete switch");
		}
	}
	// Relate all the extracted atoms with bdd variables.
	for (std::vector<KFormula*>::iterator it = atoms.begin(); it < atoms.end(); ++it) {
		if (atomsToVars.count(*it) == 0) {
			// Write the variable number onto the formula directly.
			(**it).setvar(numVars);
			atomsToVars.insert(std::pair<const KFormula*, int>(*it, numVars));
			varsToAtoms.push_back(*it);
			++numVars;
		} else {
			// Still write the variable number onto the formula directly.
			(**it).setvar(atomsToVars.at(*it));
		}
	}
	bdd_setvarnum(numVars);
	// Clear atomsToVars, it will no longer be used. Vars have been written onto the formulae.
	atomsToVars.clear();
	
	// Since the number of variables is now known, some maps from variables to 
	// other things can now be appropriately initialised.
	varsToChildren.resize(numVars + 1);
	unboxings.resize(numVars + 1);
	unboxed.resize(numVars + 1);
	undiamondings.resize(numVars + 1);
	undiamonded.resize(numVars + 1);
	
	if (bddNormalise) {
		std::unordered_map<bdd, int, BddHasher> unboxbddToVar;
		std::vector<int> varToNewVar(numVars, -1);
		// Reverse order (reverse of breadth first order in the original formula)
		for (std::vector<KFormula*>::reverse_iterator it = atoms.rbegin(); it < atoms.rend(); ++it) {
			// if AP then fine, nothing to do.
			// If box, and haven't processed yet, then unbox().
			// 		If unbox root node already has a boxVar, 
			//		Then remember this box var should be turned into that boxVar
			// 		Else record unbox root node -> boxVar.
			// Else check if this boxVar should be changed into another.
			if ((*it)->getop() == KFormula::BOX) {
				if (varToNewVar.at((*it)->getvar()) == -1) {
					// Var has not yet been assigned a new var.
					bdd unboxBDD = unbox((*it)->getvar());
					if (unboxbddToVar.count(unboxBDD) != 0) {
						// The unboxing was identical to a previous var.
						// Thus the bdds have shown they are the same box var,
						// even though syntactically different.
						varToNewVar.at((*it)->getvar()) = unboxbddToVar.at(unboxBDD);
						++numVarsReduced;
					} else {
						unboxbddToVar.insert(std::pair<bdd, int>(unboxBDD, (*it)->getvar()));
						varToNewVar.at((*it)->getvar()) = (*it)->getvar();
					}
				}
				// Var has been processed. Modify to new var.
				(*it)->setvar(varToNewVar.at((*it)->getvar()));
			}
		}
		
		if (S4) {
			// If in S4, we'll unbox by stepping past surface boxes,
			// so we don't want these 'cause they'll be different.
			unboxings.clear();
			unboxings.resize(numVars + 1);
			unboxed.clear();
			unboxed.resize(numVars + 1);
		}
	}
	
}

/*
 *	Returns the variable numbers of all atomic propositions and box formulae
 *	that are immediate subformulae of the formula represented by 'var'.
 *	
 *	Assumes formulae are in BoxNNF, and that 'var' is a box formula.
 *	
 *	Makes use of a cache of this relationship, using computeChildren() to 
 *	compute it once and then storing the result.
 */
std::unordered_set<int>& getChildren(int var) {
	if (varsToChildren.at(var).empty()) {// Surrogate for 'mapping does not exist'.
		// Get all immediate children
		// add them to the global relations.
		computeChildren(&(varsToAtoms.at(var)->getleft()), varsToChildren.at(var));
	}
	return varsToChildren.at(var);
}

/*
 *	Traverse the given formula to find all the atomic propositions and 
 *	box formulae that are immediate subformulae. (Don't go beyond any boxes)
 *
 *	Assumes the given formula is in BoxNNF, and that BDD variable numbers
 *	have been written on to the given formula.
 */
void computeChildren(const KFormula* formula, std::unordered_set<int>& children) {
	switch (formula->getop()) {
		case KFormula::AP:// Fall through
		case KFormula::BOX:
			children.insert(formula->getvar());
			break;
		case KFormula::NOT:
			// Continue getting children from past the negation.
			computeChildren(&(formula->getleft()), children);
			break;
		case KFormula::AND:// Fall through
		case KFormula::OR:
			// Left to right get from both sides.
			computeChildren(&(formula->getleft()), children);
			computeChildren(&(formula->getright()), children);
			break;
		case KFormula::TRUE:
			break;
		case KFormula::FALSE:
			break;
		case KFormula::DIA:
			assert(false && "Children getting not defined for <>.");
		case KFormula::IMP:
			assert(false && "Children getting not defined for =>.");
		case KFormula::EQU:
			assert(false && "Children getting not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/*
 *	Traverse the given formula to find all the atomic propositions and 
 *	box formulae that are immediate subformulae.
 *
 *	S4 version where intermediate boxes are ignored. Note recursion falls
 *	back to the normal version after a disjunction.
 *
 *	Assumes the given formula is in BoxNNF, and that BDD variable numbers
 *	have been written on to the given formula.
 */
void computeChildrenBoxS4(const KFormula* formula, std::unordered_set<int>& children) {
	switch (formula->getop()) {
		case KFormula::AP:// Fall through
			children.insert(formula->getvar());
			break;
		case KFormula::BOX:
			// Continue getting children from past the box.
			computeChildrenBoxS4(&(formula->getleft()), children);
			break;
		case KFormula::NOT:
			// Continue getting children from past the negation.
			// (going to be either a box or a prop, by BoxNNF)
			computeChildren(&(formula->getleft()), children);
			break;
		case KFormula::AND:// Fall through
			// Left to right get from both sides.
			computeChildrenBoxS4(&(formula->getleft()), children);
			computeChildrenBoxS4(&(formula->getright()), children);
			break;
		case KFormula::OR:
			// Left to right get from both sides.
			computeChildren(&(formula->getleft()), children);
			computeChildren(&(formula->getright()), children);
			break;
		case KFormula::TRUE:
			break;
		case KFormula::FALSE:
			break;
		case KFormula::DIA:
			assert(false && "Children getting not defined for <>.");
		case KFormula::IMP:
			assert(false && "Children getting not defined for =>.");
		case KFormula::EQU:
			assert(false && "Children getting not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/*
 *	Construct a BDD representation of the given formula.
 *	This amounts to performing the saturation phase of a tableau.
 *
 *	Assumes the given formula is in BoxNNF.
 */
bdd toBDD(const KFormula* formula) {
	switch (formula->getop()) {
		case KFormula::AP:// Fall through
		case KFormula::BOX:
			// Return the bdd to the related variable.
			return bdd_ithvar(formula->getvar());
		case KFormula::NOT:
			// Due to BoxNNF, getleft() will either be a [] or prop.
			if (formula->getleft().getop() == KFormula::BOX) {
				if (S4 || numRoles > 1 || inverseRoles) {
				    return bdd_nithvar(formula->getleft().getvar());
				} else {
					// Note the presence of a <>. Only for K, not needed for S4.
				    return bdd_nithvar(formula->getleft().getvar()) & bdd_ithvar(existsDia);
				}
			} else {// AP
				return bdd_nithvar(formula->getleft().getvar());
			}
		case KFormula::AND:
			return ( toBDD(&(formula->getleft())) & toBDD(&(formula->getright())) );
		case KFormula::OR:
			return ( toBDD(&(formula->getleft())) | toBDD(&(formula->getright())) );
		case KFormula::TRUE:
			return bddtrue;
		case KFormula::FALSE:
			return bddfalse;
		case KFormula::DIA:
			assert(false && "toBDD not defined for <>.");
		case KFormula::IMP:
			assert(false && "toBDD not defined for =>.");
		case KFormula::EQU:
			assert(false && "toBDD not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/*
 *	Construct a BDD representation of the given formula.
 *	This amounts to performing the saturation phase of a tableau.
 *	This version assumes S4, so will unbox surface boxes.
 *	After a non-conjunction, falls back to normal toBDD().
 *
 *	Will also add to the given box vars.
 *
 *	Assumes the given formula is in BoxNNF.
 */
bdd toBDDS4Unbox(const KFormula* formula) {
	switch (formula->getop()) {
		case KFormula::AP:
			// Return the bdd to the related variable.
			return bdd_ithvar(formula->getvar());
		case KFormula::BOX:
			// Return the bdd to the related variable, as well as unboxing it.
			// TODO: Determine whether the following change is valid:
			// The individual box variable is irrelevant. Completely ignore it,
			// and just continue past it as if it wasn't there.
			//boxVars.insert(formula->getvar());
			return /*bdd_ithvar(formula->getvar()) &*/ toBDDS4Unbox(&(formula->getleft()));
		case KFormula::NOT:
			// Due to BoxNNF, getleft() will either be a [] or prop.
			return bdd_nithvar(formula->getleft().getvar());
		case KFormula::AND:
			return ( toBDDS4Unbox(&(formula->getleft())) & toBDDS4Unbox(&(formula->getright())));
		case KFormula::OR:
			return ( toBDD(&(formula->getleft())) | toBDD(&(formula->getright())) );
		case KFormula::TRUE:
			return bddtrue;
		case KFormula::FALSE:
			return bddfalse;
		case KFormula::DIA:
			assert(false && "toBDDS4Unbox not defined for <>.");
		case KFormula::IMP:
			assert(false && "toBDDS4Unbox not defined for =>.");
		case KFormula::EQU:
			assert(false && "toBDDS4Unbox not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}

/*
 *	Construct a BDD representation of the negation of the given formula.
 *
 *	Due to our representation of each <>phi as []BoxNNF(~phi),
 *	a modal jump will have to compute toBDD(phi) as ~toBDD(BoxNNF(~phi)).
 *	This is a convenience function for that purpose.
 *
 *	In K, this also correctly handles the existsDia variable.
 *
 *	Assumes formula is in BoxNNF
 */
bdd toNotBDD(const KFormula* formula) {
	switch (formula->getop()) {
		case KFormula::AP:
			// Return the bdd to the negation of the related variable.
			return bdd_nithvar(formula->getvar());
		case KFormula::BOX:
			// Return the bdd to the negation of the related variable.
			if (S4 || numRoles > 1 || inverseRoles) {
			    return bdd_nithvar(formula->getvar());
			} else {
				// Note presence of a <>. Only for K, not needed for S4.
			    return bdd_nithvar(formula->getvar()) & bdd_ithvar(existsDia);
			}
		case KFormula::NOT:
			// Due to BoxNNF, getleft() will either be a [] or prop.
			return bdd_ithvar(formula->getleft().getvar());
		case KFormula::AND:// Not 'and's become 'or's.
			return ( toNotBDD(&(formula->getleft())) | toNotBDD(&(formula->getright())) );
		case KFormula::OR:// Not 'or's become 'and's.
			return ( toNotBDD(&(formula->getleft())) & toNotBDD(&(formula->getright())) );
		case KFormula::TRUE:
			return bddfalse;
		case KFormula::FALSE:
			return bddtrue;
		case KFormula::DIA:
			assert(false && "toNotBDD not defined for <>.");
		case KFormula::IMP:
			assert(false && "toNotBDD not defined for =>.");
		case KFormula::EQU:
			assert(false && "toNotBDD not defined for <=>.");
		default:
			assert(false && "Defaulted out of complete switch");
	}
}
/*
 * Wrapper for the recursive isSatsfiableK function. 
 */
bool isSatisfiable(bdd formulaBDD) {
	if (S4) {
		std::unordered_set<int> responsibleVars;
		std::unordered_set<bdd, BddHasher> assumedSatBDDs;
		bdd permanentFactsBDD = bddtrue;
		std::unordered_set<int> permanentBoxVars;
		return isSatisfiableS4(formulaBDD, responsibleVars, assumedSatBDDs,
													permanentFactsBDD, permanentBoxVars);
	} else if (inverseRoles) {
		// Not implemented
		return false;
	} else {
		std::unordered_set<int> responsibleVars;
		std::unordered_set<bdd, BddHasher> assumedSatBDDs;
		totalModalJumpsExplored++;
		return isSatisfiableK(formulaBDD, responsibleVars, assumedSatBDDs);
	}
}

/*
 *	Determine whether the formula represented by the given BDD is satisfiable
 *	or not, via the tableau method.
 *
 *	If the BDD is Unsatisfiable, 'responsibleVars' will contain a set of
 *	BDD variables representing subformulae that were directly involved in
 *	causing it to be Unsatisfiable.
 *
 *	If the BDD is Satisfiable, 'assumedSatBDDs' will contain, in BDD form,
 *	all the worlds that were assumed true to reach this result (due to cycles).
 */
bool isSatisfiableK(bdd formulaBDD, std::unordered_set<int>& responsibleVars,
std::unordered_set<bdd, BddHasher>& assumedSatBDDs) {
	
	// Statistics:
	++depth;
	if (depth > maxDepth) {
		maxDepth = depth;
	}
	
	// Sat results caching.
	if (satCache.count(formulaBDD) == 1) {
		// Then we have already proven this is Satisfiable.
		++satCacheHits;
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	}
	
	if (useSaturationUnsatCache && saturationUnsatCache.count(formulaBDD) == 1) {
		++unsatCacheHits;
		--depth;
		responsibleVars.insert(saturationUnsatCache.at(formulaBDD).begin(),
													saturationUnsatCache.at(formulaBDD).end());
		// Because resVars already includes vars from previous refinements.
		return false;
	}
    
    // Base cases:
    if (formulaBDD == bddtrue) {
    	--depth;
		++totalSatisfiableModalJumps;
    	return true;
    }
    if (formulaBDD == bddfalse) {
    	--depth;
    	return false;
    }
	
	// Get one satisfying valuation out of the formulaBDD:
	bdd satisfyingValuation;
	if (rightToLeft) {
		satisfyingValuation = rightValuation(formulaBDD);
	} else {
		satisfyingValuation = bdd_satone(formulaBDD);//leftValuation(formulaBDD);
	}
    // Make sets of the modal formulae in the satisfying valuation.
    std::vector<int> boxVars;
    std::vector<int> diaVars;
    extractSatisfyingModalVars(satisfyingValuation, boxVars, diaVars);
	
	if (diaVars.empty()) {
		// We're at an open, fully saturated tableau branch with no <> formulae.
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	} else {
		// There are <> formulae, so we must examine those modal jumps.
		
		// Record the current bdd for loop checking.
		dependentBDDs.insert(formulaBDD);
		
		// Consider each role in turn.
		for (int role = 1; role <= numRoles; ++role) {
		
			// Build unboxedBDD by unboxing the box formulae.
			bdd unboxedBDD = unsatCacheBDD & gammaBDD;// Note gamma is included here.
			for (std::vector<int>::iterator boxIt = boxVars.begin();
					boxIt != boxVars.end(); ++boxIt) {
				
				if (varsToAtoms.at(*boxIt)->getrole() != role) {
					continue;// Only looking at a particular role.
				}
		
				unboxedBDD = unboxedBDD & unbox(*boxIt);
			
				// If at any time the unboxedBDD becomes false, we can skip straight
				// to making a refinement. (bddfalse represents Unsatisfiable)
				if (unboxedBDD == bddfalse) {
					// Statistics:
					++totalModalJumpsExplored;
					if (periodicSummary && totalModalJumpsExplored % period == 0) {
						printSummaryStatistics();
					}
					++numFalseFromBox;
					numResVarsIgnoredFromBox += boxVars.size();
				
					// Find a minimal subset of the formulae unboxed so far that are
					// still Unsatisfiable.
					// Then refine over just those.
					unboxedBDD = unsatCacheBDD & gammaBDD & unbox(*boxIt);
					responsibleVars.insert(*boxIt);
					// These boxes are only unsat if there is a <> present as well.
					bdd unsatBDD = bdd_ithvar(*boxIt) & bdd_ithvar(existsDia);
					std::vector<int>::iterator endIt = boxIt;
					bdd minimalBDD = unboxedBDD;
				
					while (true) {
						unboxedBDD = minimalBDD;
						if (unboxedBDD == bddfalse) {
							// We're done, the last one added was sufficient to ensure false.
							break;
						}
						for (boxIt = boxVars.begin(); boxIt != endIt; ++boxIt) {
							if (varsToAtoms.at(*boxIt)->getrole() != role) {
								continue;// Only looking at a particular role.
							}
							unboxedBDD = unboxedBDD & unbox(*boxIt);
							if (unboxedBDD == bddfalse) {
								// Then this last one made it false.
								unsatBDD = unsatBDD & bdd_ithvar(*boxIt);
								responsibleVars.insert(*boxIt);
								endIt = boxIt;
								minimalBDD = minimalBDD & unbox(*boxIt);
								// Statistics:
								--numResVarsIgnoredFromBox;
								break;
							}
						}
					}
					unsatBDD = bdd_not(unsatBDD);
				
					return refineAndRecurse(unsatBDD, formulaBDD, responsibleVars, assumedSatBDDs);
				}
			}
		
			// Modal jump for each dia formula.
			for (std::vector<int>::iterator
					diaIt = diaVars.begin(); diaIt != diaVars.end(); ++diaIt) {
				if (varsToAtoms.at(*diaIt)->getrole() != role) {
					continue;// Only looking at a particular role.
				}
				// Statistics:
				++totalModalJumpsExplored;
				if (periodicSummary && totalModalJumpsExplored % period == 0) {
					printSummaryStatistics();
				}
			
				// Modal jumps use toNotBDD, as every <>phi is stored as []~phi.
				bdd modalJumpBDD = unboxedBDD & undiamond(*diaIt);
			
				// Check if the jump is immediately Unsatisfiable.
				if (modalJumpBDD == bddfalse) {
					// Statistics:
					++numFalseFromDia;
					numResVarsIgnoredFromDia += boxVars.size();
				
					// Then <> and some subset of []s leads to false. (and gamma).
					// Record responsible vars as only those in the subset.
					// Modal jumps use toNotBDD, as every <>phi is stored as []~phi.
					modalJumpBDD = gammaBDD & undiamond(*diaIt);
					responsibleVars.insert(*diaIt);
					// Again, <>phi are stored as []~phi, thus the nith.
					bdd unsatBDD = bdd_nithvar(*diaIt) & bdd_ithvar(existsDia);
				
					// Determine a minimal unsatisfiable subset.
					std::vector<int>::iterator endIt = boxVars.end();
					bdd minimalBDD = modalJumpBDD;
					while (true) {
						modalJumpBDD = minimalBDD;
						if (modalJumpBDD == bddfalse) {
							// We're done, the last one added was sufficient to ensure false.
							break;
						} else {
							for (std::vector<int>::iterator boxIt = boxVars.begin(); boxIt != endIt; ++boxIt) {
								if (varsToAtoms.at(*boxIt)->getrole() != role) {
									continue;// Only looking at a particular role.
								}
								modalJumpBDD = modalJumpBDD & unbox(*boxIt);
								if (modalJumpBDD == bddfalse) {
									// The last [] introduced the false. Add it to the minimal set, and start again.
									minimalBDD = minimalBDD & unbox(*boxIt);
									unsatBDD = unsatBDD & bdd_ithvar(*boxIt);
									responsibleVars.insert(*boxIt);
									endIt = boxIt;// No need to go beyond this one again.
									// Statistics:
									--numResVarsIgnoredFromDia;
									break;
								}
							}
						}
					}
				
					unsatBDD = bdd_not(unsatBDD);
				
					return refineAndRecurse(unsatBDD, formulaBDD, responsibleVars, assumedSatBDDs);
				}
			
				// Check for loops:
				if (dependentBDDs.count(modalJumpBDD) == 1) {
					// Then we have a cyclic dependency.
					// Assume that the cycle bdd is satisfiable and continue.
					// Prevent any Sat caching that relies on this assumption.
					assumedSatBDDs.insert(modalJumpBDD);
					everAssumedSatBDDs.insert(modalJumpBDD);
					// Statistics:
					++loopsDetected;
					continue;// To the next modal jump.
				}
			
				// Make space for getting the responsible vars and assumedSatBDDs.
				std::unordered_set<int> postModalJumpResVars;
				std::unordered_set<bdd, BddHasher> postModalJumpAssumedSatBDDs;
			
				// See if we can apply any element of the unsat cache to this new
				// modal jump.
				std::unordered_set<int> cacheResVars;
				if (useUnsatCache && !bddUnsatCache && !useSaturationUnsatCache) {// If using this style of cache.
					// If the unsat cache is empty, don't bother.
					if (!unsatCache.empty()) {
			
						std::unordered_set<int> modalJumpModalVars = getModalVars(modalJumpBDD);
				
						for (std::map<std::vector<int>, bdd>::iterator unsatIt = unsatCache.begin();
								unsatIt != unsatCache.end(); ++unsatIt) {
						
							if (isSubset(unsatIt->first, modalJumpModalVars)) {
								modalJumpBDD = modalJumpBDD & unsatIt->second;
								cacheResVars.insert(unsatIt->first.begin(), unsatIt->first.end());
								// Statistics:
								++unsatCacheHits;
								if (modalJumpBDD == bddfalse) {
									break;
								}
							}
						}
					}
			
					// Check for loops again after unsat cache:
					if (dependentBDDs.count(modalJumpBDD) == 1) {
						// Then we have a cyclic dependency.
						// Assume that the cycle bdd is satisfiable and continue.
						// Prevent any Sat caching that relies on this assumption.
						assumedSatBDDs.insert(modalJumpBDD);
						everAssumedSatBDDs.insert(modalJumpBDD);
						// Statistics:
						++loopsDetected;
						continue;// To the next modal jump.
					}
				}
			
				// Check Satisfiability of the modal jump:
				if (!isSatisfiableK(modalJumpBDD, postModalJumpResVars, postModalJumpAssumedSatBDDs)) {
					// Unsatisfiable:
					// Then we want to modify the bdd to remove this branch,
					// and recurse with that.
					// Only refine over the []/<> variables that introduced a ResponsibleVariable 
					// from the modal jump. But if a variable introduces one of those, we'll have
					// to consider the other variables it introduces as potentially responsible as well.
				
					// Add in resVars from the unsatCache process
					postModalJumpResVars.insert(cacheResVars.begin(), cacheResVars.end());
				
					// Statistics:
					numResVarsIgnoredFromGeneral += boxVars.size() + 1;
					
					// If using this style of unsat cache
					if (useSaturationUnsatCache) {
						cacheUnsat(postModalJumpResVars, modalJumpBDD);
					}
					
					bdd unsatBDD = bdd_ithvar(existsDia);
					bool newPostModalJumpResVarsAdded = true;
					while (newPostModalJumpResVarsAdded) {
						newPostModalJumpResVarsAdded = false;
						for (std::vector<int>::iterator boxIt = boxVars.begin(); boxIt != boxVars.end(); ++boxIt) {
							if (varsToAtoms.at(*boxIt)->getrole() != role) {
								continue;// Only looking at a particular role.
							}
							if (responsibleVars.count(*boxIt) != 0) {
								// Don't bother, we've already accounted for this box var.
							} else if (shareAnElement(postModalJumpResVars, getChildren(*boxIt))) {
								unsatBDD = unsatBDD & bdd_ithvar(*boxIt);
								responsibleVars.insert(*boxIt);
								// Add in other children to postModalJumpResVars
								postModalJumpResVars.insert(getChildren(*boxIt).begin(),
															getChildren(*boxIt).end());
								newPostModalJumpResVarsAdded = true;
								// Statistics:
								--numResVarsIgnoredFromGeneral;
							}
						}
						if (responsibleVars.count(*diaIt) != 0) {
							// Don't bother, we've already accounted for this dia var.
						} else if (shareAnElement(postModalJumpResVars, getChildren(*diaIt))) {
							unsatBDD = unsatBDD & bdd_nithvar(*diaIt);// Note, stored as [] formula, thus the nith.
							responsibleVars.insert(*diaIt);
							// Add in other children to postModalJumpResVars
							postModalJumpResVars.insert(getChildren(*diaIt).begin(),
														getChildren(*diaIt).end());
							newPostModalJumpResVarsAdded = true;
							// Statistics:
							--numResVarsIgnoredFromGeneral;
						}
					}
					unsatBDD = bdd_not(unsatBDD);
				
					return refineAndRecurse(unsatBDD, formulaBDD, responsibleVars, assumedSatBDDs);
				}
			
				// Modal jump was Satisfiable:
				// Accumulate assumedSatBDDS, if there were any.
				assumedSatBDDs.insert(postModalJumpAssumedSatBDDs.begin(), postModalJumpAssumedSatBDDs.end());
			
				// Continue to the next modal jump
			}
		}
		
		// All modal jumps were satisfiable:
		// If currently assumed Sat, this bdd now has a value and need not be assumed.
		if (assumedSatBDDs.count(formulaBDD) == 1) {
			assumedSatBDDs.erase(formulaBDD);
		}
		// Discharge any Sat assumptions of this bdd.
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			confirmSatAssumption(formulaBDD);
		}
		cacheSat(formulaBDD, assumedSatBDDs);
		dependentBDDs.erase(formulaBDD);
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	}
	
}

bool refineAndRecurse(bdd& unsatBDD, bdd& formulaBDD, std::unordered_set<int>& responsibleVars,
						std::unordered_set<bdd, BddHasher>& assumedSatBDDs) {
	if (!useSaturationUnsatCache) {// Only if we're using this style of cache.
		// Cache this unsatisfiable branch
		cacheUnsat(responsibleVars, unsatBDD);
	}
	
	// Perform the refinement:
	bdd refinedBDD = formulaBDD & unsatBDD;
	
	// Statistics:
	++totalBDDRefinements;
	
	// Ignore accumulated assumptions, as we are now looking at a new branch.
	assumedSatBDDs.clear();

	// Make space for getting the responsible vars.
	std::unordered_set<int> postRefinementResVars;

	bool isSat;
	// Catch refinement immediate unsatisfiability.
	if (refinedBDD == bddfalse) {
		++numFalseFromRef;
		isSat = false;
	}// Check for loops here as well:
	else if (dependentBDDs.count(refinedBDD) == 1) {
		// Then we have a cyclic dependency.
		// Assume that the cycle bdd is satisfiable and continue.
		// Prevent any Sat caching that relies on this assumption.
		assumedSatBDDs.insert(refinedBDD);
		everAssumedSatBDDs.insert(refinedBDD);
		// Statistics:
		++loopsDetected;
		isSat = true;
	} else {// Determine the satisfiability of the refined bdd.
		// Statisticis:
		--depth;
		isSat = isSatisfiableK(refinedBDD, postRefinementResVars, assumedSatBDDs);
		++depth;
	}
	
	// If currently assumed Sat, this bdd now has a value and need not be assumed.
	if (assumedSatBDDs.count(formulaBDD) == 1) {
		assumedSatBDDs.erase(formulaBDD);
	}

	if (isSat) {
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			// Discharge any Sat assumptions of this bdd.
			confirmSatAssumption(formulaBDD);
		}
		cacheSat(formulaBDD, assumedSatBDDs);
	} else {
		// Pass back responsible variables from all refinements.
		responsibleVars.insert(postRefinementResVars.begin(), postRefinementResVars.end());
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			// Reject any Sat assumptions of this bdd.
			rejectSatAssumption(formulaBDD);
		}
	}
	--depth;
	dependentBDDs.erase(formulaBDD);
	return isSat;
}

/* Takes a satisfying valuation of a bdd (ie from bdd_satone),
 * and extracts the corresponding modal variables, putting them into the given sets.
 * (Non-modal vars are ignored.)
 * (Assumes BoxNNF, ie all modal formula represented by []s.)
 */
void extractSatisfyingModalVars (bdd satValuation,
		std::vector<int>& extBoxVars,
		std::vector<int>& extDiaVars) {
	while (satValuation != bddtrue) {
		if (bdd_low(satValuation) == bddfalse) {
			// Only consider the modal ones: (and ignore the existsDia var)
			if (bdd_var(satValuation) != existsDia
			&& varsToAtoms.at(bdd_var(satValuation))->getop() == KFormula::BOX) {
				// Formula is true in the valuation:
				extBoxVars.push_back(bdd_var(satValuation));
			}
			// Extract formulae from the rest of the satisfying valuation.
			satValuation = bdd_high(satValuation);
		} else {//bdd_high == bddfalse, by defn of satone.
			// Only consider the modal ones: (and ignore the existsDia var)
			if (bdd_var(satValuation) != existsDia
			&& varsToAtoms.at(bdd_var(satValuation))->getop() == KFormula::BOX) {
				// Formula was false in the valuation.
				extDiaVars.push_back(bdd_var(satValuation));
			}
			// Extract formulae from the rest of the satisfying valuation.
			satValuation = bdd_low(satValuation);
		}
	}
}

/*
 *	Cache a Satisfiable result.
 */
void cacheSat(bdd& b, std::unordered_set<bdd, BddHasher>& assumedSatBDDs) {
	if (assumedSatBDDs.empty()) {
		if (satCache.size() >= maxCacheSize) {
			// Remove one element, in a FIFO fashion.
			satCache.erase(satCacheDeque.front());
			satCacheDeque.pop_front();
		}
		if (satCache.count(b) == 0) {
			satCacheDeque.push_back(b);
			satCache.insert(b);
			// Statistics:
			++satCacheAdds;
		}
	} else {
		if (tempSatCaches.size() < maxCacheSize) {
			tempSatCaches.push_back(std::pair<std::unordered_set<bdd, BddHasher>, bdd>(assumedSatBDDs, b));
			// Statistics:
			++numTempSatCaches;
		}
	}
}

/*
 *	Cache an Unsatisfiable result.
 */
void cacheUnsat(std::unordered_set<int>& vars, bdd& b) {
	if (useUnsatCache) {
		if (bddUnsatCache) {
			unsatCacheBDD = unsatCacheBDD & b;
		} else if (useSaturationUnsatCache) {
			// Cache the saturation phase bdd, not the refinement bdd.
			if (saturationUnsatCache.size() >= maxCacheSize) {
				// Remove one element, in a FIFO fashion.
				saturationUnsatCache.erase(saturationUnsatCacheDeque.front());
				saturationUnsatCacheDeque.pop_front();
			}
			
			saturationUnsatCache.insert(std::pair<bdd, std::unordered_set<int>>(b, vars));
			saturationUnsatCacheDeque.push_back(b);
		} else {
			if (unsatCache.size() >= maxCacheSize) {
				// Remove one element, in a FIFO fashion.
				unsatCache.erase(unsatCacheDeque.front());
				unsatCacheDeque.pop_front();
			}
			// Make an ordered vector from the unordered_set of vars.
			std::vector<int> orderedVars;
			orderedVars.insert(orderedVars.begin(), vars.begin(), vars.end());
			std::sort(orderedVars.begin(), orderedVars.end());
			if (unsatCache.count(orderedVars) != 0) {
				// Cache already contains these vars.
				// Can get here from dia or box instafalse, ie false before checking the
				// unsat cache.
			} else {
				unsatCache.insert(std::pair<std::vector<int>, bdd>(orderedVars, b));
				unsatCacheDeque.push_back(orderedVars);
			}
		}
		// Statistics:
		++unsatCacheAdds;
	}
}


/*
 *	Determine whether two sets share any elements.
 */
bool shareAnElement(const std::unordered_set<int>& firstSet,
					const std::unordered_set<int>& secondSet) {
	if (firstSet.size() < secondSet.size()) {
		for (std::unordered_set<int>::const_iterator it = firstSet.begin(); it != firstSet.end(); ++it) {
			if (secondSet.count(*it) != 0) {
				return true;
			}
		}
		return false;
	} else {
		for (std::unordered_set<int>::const_iterator it = secondSet.begin(); it != secondSet.end(); ++it) {
			if (firstSet.count(*it) != 0) {
				return true;
			}
		}
		return false;
	}
}

/*
 *	Get all the variables from the given BDD that represent modal formulae.
 */
std::unordered_set<int> getModalVars(bdd& b) {
	bdd support = bdd_support(b);
	std::unordered_set<int> modalVars;
	if (b != bddtrue && b != bddfalse) {
		extractModalVars(support, modalVars);
	}
	return modalVars;
}
void extractModalVars(bdd support, std::unordered_set<int>& modalVars) {
	if (support == bddtrue) {
		return;
	}
	if (bdd_var(support) != existsDia// existsDia needs to be explicitly handled.
	&& varsToAtoms.at(bdd_var(support))->getop() == KFormula::BOX) {
		modalVars.insert(bdd_var(support));
	}
	extractModalVars(bdd_high(support), modalVars);
}

/*
 *	Determine whether the given vector is a subset of the given set.
 */
bool isSubset(std::vector<int> vector, std::unordered_set<int>& set) {
	if (vector.size() > set.size()) {
		return false;
	}
	for (size_t i = 0; i < vector.size(); ++i) {
		if (set.count(vector.at(i)) == 0) {
			return false;
		}
	}
	return true;
}

/*
 *	Confirm that a BDD that was assumed Satisfiable is actually Satisfiable.
 */
void confirmSatAssumption(bdd& b) {
	// Go through tempSatCaches
	for (std::list<std::pair<std::unordered_set<bdd, BddHasher>, bdd>>
			::iterator it = tempSatCaches.begin();
			it != tempSatCaches.end(); ++it) {
		// eliminate b from any tempSatCaches that contain it.
		if (it->first.count(b) == 1) {
			it->first.erase(b);
			// If they don't have any assumptions any more, transfer from temp to real cache.
			if (it->first.empty()) {
				// Empty assumptions for using cacheSat
				std::unordered_set<bdd, BddHasher> noAssumptions;
				cacheSat(it->second, noAssumptions);
				// Remove from temp cache
				it = tempSatCaches.erase(it);
				--it;
				// Statistics:
				++tempSatCachesConfirmed;
			}
		}
	}
	everAssumedSatBDDs.erase(b);
}

/*
 *	Reject the assumption that the given BDD was Satisfiable, becuase it has been
 *	found to be Unsatisfiable.
 */
void rejectSatAssumption(bdd& b) {
	// Go through tempSatCaches
	for (std::list<std::pair<std::unordered_set<bdd, BddHasher>, bdd>>
			::iterator it = tempSatCaches.begin();
			it != tempSatCaches.end(); ++it) {
		// eliminate entire caches if they assumed b, as b is unsat.
		if (it->first.count(b) == 1) {
			it = tempSatCaches.erase(it);
			--it;
			++tempSatCachesRejected;
		}
	}
	everAssumedSatBDDs.erase(b);
}


/*
 *	Determine whether the formula represented by the given BDD is satisfiable
 *	or not, via the tableau method, with a reflexive and transitive relation.
 *
 *	If the BDD is Unsatisfiable, 'responsibleVars' will contain a set of
 *	BDD variables representing subformulae that were directly involved in
 *	causing it to be Unsatisfiable.
 *
 *	If the BDD is Satisfiable, 'assumedSatBDDs' will contain, in BDD form,
 *	all the worlds that were assumed true to reach this result (due to cycles).
 *
 *	The permanent variables denote the [] formulae, and the result of their
 *	unboxing, that will be true at all subsequent worlds, due to the 
 *	transitivity of the relation.
 */
bool isSatisfiableS4(bdd formulaBDD, std::unordered_set<int>& responsibleVars,
					 std::unordered_set<bdd, BddHasher>& assumedSatBDDs,
					 bdd permanentFactsBDD, 
					 std::unordered_set<int> permanentBoxVars) {


	// Statistics:
	++depth;
	if (depth > maxDepth) {
		maxDepth = depth;
	}

	// Sat results caching.
	if (satCache.count(formulaBDD) == 1) {
		// Then we have already proven this is Satisfiable.
		++satCacheHits;
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	}
	
	// Unsat results caching.
	if (useSaturationUnsatCache && saturationUnsatCache.count(formulaBDD) == 1) {
		// Then we have already proven this is Unsatisfiable.
		++unsatCacheHits;
		--depth;
		responsibleVars.insert(saturationUnsatCache.at(formulaBDD).begin(),
													saturationUnsatCache.at(formulaBDD).end());
		return false;
	}

	// Base cases:
	if (formulaBDD == bddtrue) {
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	}
	if (formulaBDD == bddfalse) {
		--depth;
		return false;
	}
	
	// Get one satisfying valuation out of the formulaBDD:
	bdd satisfyingValuation;
	if (rightToLeft) {
		satisfyingValuation = rightValuation(formulaBDD);
	} else {
		satisfyingValuation = bdd_satone(formulaBDD);//leftValuation(formulaBDD);
	}
	
	// Get sets of the modal formulae in the satisfying valuation.
	std::vector<int> boxVars;
	std::vector<int> diaVars;
	extractSatisfyingModalVars(satisfyingValuation, boxVars, diaVars);
	
	
	// If we have new box vars to unbox, unbox them and recurse.
	bdd postUnboxingPermanentFactsBDD = permanentFactsBDD;
	std::unordered_set<int> postUnboxingPermanentBoxVars = permanentBoxVars;
	bdd satValWithUnboxedBDD = satisfyingValuation;
	std::vector<int> newBoxVars;
	for (std::vector<int>::iterator boxIt = boxVars.begin(); boxIt != boxVars.end(); ++boxIt) {
		if (postUnboxingPermanentBoxVars.count(*boxIt) == 0) {
			// unbox, record the new var, add to permaFacts, permaVars and
			// satValWithUnboxed
			newBoxVars.push_back(*boxIt);
			bdd unboxedBDD = unboxS4(*boxIt);//toBDDS4Unbox(&((varsToAtoms.at(*boxIt))->getleft()), postUnboxingPermanentBoxVars);
			postUnboxingPermanentFactsBDD = postUnboxingPermanentFactsBDD & unboxedBDD & bdd_ithvar(*boxIt);
			postUnboxingPermanentBoxVars.insert(*boxIt);
			satValWithUnboxedBDD = satValWithUnboxedBDD & unboxedBDD;
			
			if (satValWithUnboxedBDD == bddfalse) {
				// Statistics:
				++numFalseFromBox;
				
				// Determine a minimal set of newBoxVars that lead to the false.
				// The last one was definitely necessary:
				bdd minBoxVarsBDD = unboxedBDD;
				satValWithUnboxedBDD = satisfyingValuation & unboxedBDD;
				responsibleVars.insert(*boxIt);
				bdd unsatBDD = bdd_ithvar(*boxIt);
				// Find a minimal set of previous ones that are necessary.
				std::vector<int>::iterator endIt = --(newBoxVars.end());
				bdd minimalBDD = satValWithUnboxedBDD;
				while (true) {
					satValWithUnboxedBDD = minimalBDD;
					if (satValWithUnboxedBDD == bddfalse) {
						// We're done, the last one added to minimal was
						// sufficient to ensure false, we have a minimal set.
						break;
					}
					for (std::vector<int>::iterator newBoxIt = newBoxVars.begin();
							newBoxIt != endIt; ++newBoxIt) {
						satValWithUnboxedBDD = satValWithUnboxedBDD
												& unboxS4(*newBoxIt);//toBDDS4Unbox(&((varsToAtoms.at(*newBoxIt))->getleft()), postUnboxingPermanentBoxVars);
						if (satValWithUnboxedBDD == bddfalse) {
							// Then this last one made it false.
							minimalBDD = minimalBDD & unboxS4(*newBoxIt);//toBDDS4Unbox(&((varsToAtoms.at(*newBoxIt))->getleft()), postUnboxingPermanentBoxVars);
							minBoxVarsBDD = minBoxVarsBDD & unboxS4(*newBoxIt);//toBDDS4Unbox(&((varsToAtoms.at(*newBoxIt))->getleft()), postUnboxingPermanentBoxVars);
							unsatBDD = unsatBDD & bdd_ithvar(*newBoxIt);
							responsibleVars.insert(*newBoxIt);
							endIt = newBoxIt;
							break;
						}
					}
				}
				
				// Determine a minimal set of other satVal vars that lead
				// to false with the minimal set of newboxVars.
				std::vector<std::pair<int, bool>> satValVars;
				extractAllVars(satisfyingValuation, satValVars);
				std::vector<std::pair<int, bool>>::iterator satEndIt = satValVars.end();
				minimalBDD = minBoxVarsBDD;
				while (true) {
					minBoxVarsBDD = minimalBDD;
					if (minBoxVarsBDD == bddfalse) {
						// We're done, the last one added was sufficient to ensure false,
						// we have a minimal set.
						break;
					}
					for (std::vector<std::pair<int, bool>>::iterator varIt = satValVars.begin();
							varIt != satEndIt; ++varIt) {
						if (varIt->second == true) {
							minBoxVarsBDD = minBoxVarsBDD & bdd_ithvar(varIt->first);
						} else {
							minBoxVarsBDD = minBoxVarsBDD & bdd_nithvar(varIt->first);
						}
						if (minBoxVarsBDD == bddfalse) {
							// Then this last one made it false.
							if (varIt->second == true) {
								minimalBDD = minimalBDD & bdd_ithvar(varIt->first);
								unsatBDD = unsatBDD & bdd_ithvar(varIt->first);
							} else {
								minimalBDD = minimalBDD & bdd_nithvar(varIt->first);
								unsatBDD = unsatBDD & bdd_nithvar(varIt->first);
							}
							responsibleVars.insert(varIt->first);
							satEndIt = varIt;
							break;
						}
					}
				}
				
				unsatBDD = bdd_not(unsatBDD);
				
				return refineAndRecurseS4(unsatBDD, formulaBDD, responsibleVars,
							assumedSatBDDs, permanentFactsBDD, permanentBoxVars);
			}
		}
	}
	if (!newBoxVars.empty()) {
		// We performed an unboxing phase, and should recurse.
		
		// Record the current bdd for loop checking.
		dependentBDDs.insert(formulaBDD);
		
		// Make space for getting the responsibleVars and assumedSatBDDs.
		std::unordered_set<int> postUnboxingResVars;
		std::unordered_set<bdd, BddHasher> postUnboxingAssumedSatBDDs;
		
		// And check for unsatisfiability of this branch with the
		// newly unboxed formulae.
		// Depth stat:
		--depth;
		bool unboxedSat = isSatisfiableS4(satValWithUnboxedBDD, postUnboxingResVars,
				postUnboxingAssumedSatBDDs, postUnboxingPermanentFactsBDD,
				postUnboxingPermanentBoxVars);
		++depth;
		
		if (!unboxedSat) {
			// If Unsatisfiable:
			// Then we want to modify the bdd, and recurse with that.
			
			// Here, the res vars are local vars, so generally aren't interested
			// in children.
			// Except for those box vars we unboxed. If their children were involved,
			// then they were involved.
			std::vector<std::pair<int, bool>> satValVars;
			extractAllVars(satisfyingValuation, satValVars);
			
			bdd unsatBDD = bddtrue;
			for (std::vector<std::pair<int, bool>>::iterator varIt = satValVars.begin();
					varIt != satValVars.end(); ++varIt) {
				// If a var is a responsible var
				if (postUnboxingResVars.count(varIt->first) == 1) {
					if (varIt->second == true) {
						unsatBDD = unsatBDD & bdd_ithvar(varIt->first);
					} else {
						unsatBDD = unsatBDD & bdd_nithvar(varIt->first);
					}
					responsibleVars.insert(varIt->first);
				}
			}
			for (std::vector<int>::iterator newBoxIt = newBoxVars.begin();
					newBoxIt != newBoxVars.end(); ++newBoxIt) {
				// See if any of it's children were involved.
				// That would mean that this var is responsible.
				std::unordered_set<int> children;// TODO temporary fix, ned to add caching back in.
				computeChildrenBoxS4(&(varsToAtoms.at(*newBoxIt)->getleft()), children);
				if (shareAnElement(postUnboxingResVars, children)) {
					unsatBDD = unsatBDD & bdd_ithvar(*newBoxIt);
					responsibleVars.insert(*newBoxIt);
				}
			}
			unsatBDD = bdd_not(unsatBDD);
			
			return refineAndRecurseS4(unsatBDD, formulaBDD, responsibleVars,
							assumedSatBDDs, permanentFactsBDD, permanentBoxVars);
		}
		
		// Unboxing was satisfiable:
		// If currently assumed Sat, it now has a value and need not be assumed.
		if (assumedSatBDDs.count(formulaBDD) == 1) {
			assumedSatBDDs.erase(formulaBDD);
		}
		// Discharge any Sat assumptions of this bdd.
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			confirmSatAssumption(formulaBDD);
		}
		cacheSat(formulaBDD, assumedSatBDDs);
		dependentBDDs.erase(formulaBDD);
		--depth;
		return true;
		
	}

	// No unboxing was necessary:
    
	if (diaVars.empty()) {
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	} else {
		
		// Record the current bdd for loop checking.
		dependentBDDs.insert(formulaBDD);
		
		// Modal jump for each dia formula.
		for (std::vector<int>::iterator
				diaIt = diaVars.begin(); diaIt != diaVars.end(); ++diaIt) {
			// Statistics:
			++totalModalJumpsExplored;
			if (periodicSummary && totalModalJumpsExplored % period == 0) {
				printSummaryStatistics();
			}
			
			// Modal jumps use toNotBDD, as <>phi are stored as []~phi.
			bdd modalJumpBDD = unsatCacheBDD & gammaBDD & permanentFactsBDD
								& undiamond(*diaIt);
								
			// Check for immediate Unsatisfiability of the modal jump.
			if (modalJumpBDD == bddfalse) {
				// Statistics:
				++numFalseFromDia;
				numResVarsIgnoredFromDia += permanentBoxVars.size();
				
				// Then <> and some subset of []s in permanentFacts (and gamma)
				// leads to false.
				// Record responsible vars as only those in the subset.
				// Modal jumps use toNotBDD, as <>phi are stored as []~phi.
				modalJumpBDD = unsatCacheBDD & gammaBDD & undiamond(*diaIt);
				responsibleVars.insert(*diaIt);
				// Again, <>phi are stored as []~phi, thus the nith.
				bdd unsatBDD = bdd_nithvar(*diaIt);
				
				// Determine a minimal unsatisfiable subset.
				std::unordered_set<int>::iterator endIt = permanentBoxVars.end();
				bdd minimalBDD = modalJumpBDD;
				while (true) {
					modalJumpBDD = minimalBDD;
					if (modalJumpBDD == bddfalse) {
						// We're done, the last one added was sufficient to ensure false.
						break;
					} else {
						for (std::unordered_set<int>::iterator
								boxIt = permanentBoxVars.begin();
								boxIt != endIt; ++boxIt) {
							modalJumpBDD = modalJumpBDD & unboxS4(*boxIt)
											& bdd_ithvar(*boxIt);
							if (modalJumpBDD == bddfalse) {
								// The last [] introduced the false. Add it to the minimal set, and start again.
								minimalBDD = minimalBDD & unboxS4(*boxIt)
													& bdd_ithvar(*boxIt);
								unsatBDD = unsatBDD & bdd_ithvar(*boxIt);
								responsibleVars.insert(*boxIt);
								endIt = boxIt;// No need to go beyond this one again.
								// Statistics:
								--numResVarsIgnoredFromDia;
								break;
							}
						}
					}
				}
				
				unsatBDD = bdd_not(unsatBDD);
				
				if (!useSaturationUnsatCache) {
					// Cache this unsatisfiable branch
					cacheUnsat(responsibleVars, unsatBDD);
	      }
	            
				return refineAndRecurseS4(unsatBDD, formulaBDD, responsibleVars,
										  assumedSatBDDs, permanentFactsBDD,
										  permanentBoxVars);
			}
			
			// Check for loops:
			if (dependentBDDs.count(modalJumpBDD) == 1) {
				// Then we have a cyclic dependency.
				// Assume that the cycle bdd is satisfiable and continue.
				// Prevent any Sat caching that relies on this assumption.
				assumedSatBDDs.insert(modalJumpBDD);
				everAssumedSatBDDs.insert(modalJumpBDD);
				// Statistics:
				++loopsDetected;
				continue;// To the next modal jump.
			}
			
			// See if we can apply any cached Unsat results:
			std::unordered_set<int> cacheResVars;
			if (useUnsatCache && !bddUnsatCache) {// if using this style of cache.
				// If the unsat cache is empty, don't bother.
				if (!unsatCache.empty()) {
					std::unordered_set<int> modalJumpModalVars = getModalVars(modalJumpBDD);
					for (std::map<std::vector<int>, bdd>::iterator unsatIt = unsatCache.begin();
					unsatIt != unsatCache.end(); ++unsatIt) {
						if (isSubset(unsatIt->first, modalJumpModalVars)) {
							modalJumpBDD = modalJumpBDD & unsatIt->second;
							cacheResVars.insert(unsatIt->first.begin(), unsatIt->first.end());
							++unsatCacheHits;
							if (modalJumpBDD == bddfalse) {
								break;
							}
						}
					}
				}
			
				// Check for loops again after the unsat cache application:
				if (dependentBDDs.count(modalJumpBDD) == 1) {
					// Then we have a cyclic dependency.
					// Assume that the cycle bdd is satisfiable and continue.
					// Prevent any Sat caching that relies on this assumption.
					assumedSatBDDs.insert(modalJumpBDD);
					everAssumedSatBDDs.insert(modalJumpBDD);
					// Statistics:
					++loopsDetected;
					continue;// To the next modal jump.
				}
			}
			
			// Make space for getting the responsibleVars and assumedSatBDDs.
			std::unordered_set<int> postModalJumpResVars;
			std::unordered_set<bdd, BddHasher> postModalJumpAssumedSatBDDs;
			
			// And check for unsatisfiability of the world beyond the modal jump.
			if (!isSatisfiableS4(modalJumpBDD, postModalJumpResVars,
					postModalJumpAssumedSatBDDs, permanentFactsBDD, permanentBoxVars)) {
				// Unsatisfiable:
				// Then we want to modify the bdd, and recurse with that.
				
				// Include resVars we used from the unsat cache
				postModalJumpResVars.insert(cacheResVars.begin(), cacheResVars.end());
				
				// If using a saturation style cache
				if (useSaturationUnsatCache) {
					cacheUnsat(postModalJumpResVars, modalJumpBDD);
				}
				
				// Only refine over variables that introduce a responsible variable.
				// To handle reflexivity, []phi introduce themselves.
				// If a var introduces a resVar and other vars, those vars
				// become responsible as well.
				// Statistics:
				numResVarsIgnoredFromGeneral += permanentBoxVars.size() + 1;
				
				bdd unsatBDD = bddtrue;
				bool newPostModalJumpResVarsAdded = true;
				while (newPostModalJumpResVarsAdded) {
					newPostModalJumpResVarsAdded = false;
					for (std::unordered_set<int>::iterator boxIt = permanentBoxVars.begin();
							boxIt != permanentBoxVars.end(); ++boxIt) {
						if (responsibleVars.count(*boxIt) != 0) {
							// Don't bother, we've already accounted for this box var.
							continue;
						}
						std::unordered_set<int> children = getChildren(*boxIt);
						children.insert(*boxIt);
						if (shareAnElement(postModalJumpResVars, children)) {
							unsatBDD = unsatBDD & bdd_ithvar(*boxIt);
							responsibleVars.insert(*boxIt);
							// Add in other children to postModalJumpResVars
							postModalJumpResVars.insert(children.begin(),
														children.end());
							newPostModalJumpResVarsAdded = true;
							// Statistics:
							--numResVarsIgnoredFromGeneral;
						}
					}
					if (responsibleVars.count(*diaIt) != 0) {
						// Don't bother, we've already accounted for this dia var.
					} else if (shareAnElement(postModalJumpResVars, getChildren(*diaIt))) {
						// Note, <>phi stored as []~phi, thus the nith.
						unsatBDD = unsatBDD & bdd_nithvar(*diaIt);
						responsibleVars.insert(*diaIt);
						// Add in other children to postModalJumpResVars
						postModalJumpResVars.insert(getChildren(*diaIt).begin(),
													getChildren(*diaIt).end());
						newPostModalJumpResVarsAdded = true;
						// Statistics:
						--numResVarsIgnoredFromGeneral;
					}
				}
				unsatBDD = bdd_not(unsatBDD);
				
	      if (!useSaturationUnsatCache) {
					// Cache this unsatisfiable branch
					cacheUnsat(responsibleVars, unsatBDD);
	      }
	      
				return refineAndRecurseS4(unsatBDD, formulaBDD, responsibleVars,
										  assumedSatBDDs, permanentFactsBDD,
										  permanentBoxVars);
			}
			
			// Modal jump was Satisfiable:
			// Accumulate assumedSatBDDS
			assumedSatBDDs.insert(postModalJumpAssumedSatBDDs.begin(), postModalJumpAssumedSatBDDs.end());
			
			// Continue to the next modal jump.
		}
		
		// All modal jumps were satisfiable:
		// If currently assumed Sat, this BDD now has a value and need not be assumed.
		if (assumedSatBDDs.count(formulaBDD) == 1) {
			assumedSatBDDs.erase(formulaBDD);
		}
		// Discharge any Sat assumptions of this bdd.
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			confirmSatAssumption(formulaBDD);
		}
		cacheSat(formulaBDD, assumedSatBDDs);
		dependentBDDs.erase(formulaBDD);
		--depth;
		++totalSatisfiableModalJumps;
		return true;
	}
}


bool refineAndRecurseS4(bdd& unsatBDD, bdd& formulaBDD,
						std::unordered_set<int>& responsibleVars,
						std::unordered_set<bdd, BddHasher>& assumedSatBDDs,
						bdd& permanentFactsBDD,
						std::unordered_set<int>& permanentBoxVars) {
	
	// Perform the refinement:
	bdd refinedBDD = formulaBDD & unsatBDD;
	
	// Statistics:
	++totalBDDRefinements;
	
	// Ignore accumulated assumptions. We are now exploring a new branch.
	assumedSatBDDs.clear();

	// Make space for getting the responsible vars.
	std::unordered_set<int> postRefinementResVars;

	bool isSat;
	// Catch refinement immediate Unsatisfiability.
	if (refinedBDD == bddfalse) {
		++numFalseFromRef;
		isSat = false;
	}// Check for loops here as well:
	else if (dependentBDDs.count(refinedBDD) == 1) {
		// Then we have a cyclic dependency.
		// Assume that the cycle bdd is satisfiable and continue.
		// Prevent any Sat caching that relies on this assumption.
		assumedSatBDDs.insert(refinedBDD);
		everAssumedSatBDDs.insert(refinedBDD);
		// Statistics:
		++loopsDetected;
		isSat = true;
	} else {// Go determine the satisfiability of the refined bdd.
		// Statistics:
		--depth;
		isSat = isSatisfiableS4(refinedBDD, postRefinementResVars, assumedSatBDDs,
								permanentFactsBDD, permanentBoxVars);
		++depth;
	}
	
	// If currently assumed Sat, this BDD now has a value and need not be assumed.
	if (assumedSatBDDs.count(formulaBDD) == 1) {
		assumedSatBDDs.erase(formulaBDD);
	}

	if (isSat) {
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			// Discharge any Sat assumptions of this bdd.
			confirmSatAssumption(formulaBDD);
		}
		cacheSat(formulaBDD, assumedSatBDDs);
	} else {
		// Pass back responsible variables from all refinements.
		responsibleVars.insert(postRefinementResVars.begin(), postRefinementResVars.end());
		if (everAssumedSatBDDs.count(formulaBDD) == 1) {
			// Reject any Sat assumptions of this bdd.
			rejectSatAssumption(formulaBDD);
		}
	}
	--depth;
	dependentBDDs.erase(formulaBDD);
	return isSat;
}


/*
 *	Get all variables (and their values) from a satisfying valuation, into
 *	a more convenient form.
 *	Don't pass bddtrue of bddfalse here. It's not handled like that.
 */
void extractAllVars(bdd satValuation,
					std::vector<std::pair<int, bool>>& satValVars) {
	if (bdd_low(satValuation) == bddfalse) {
		// ignore the existsDia var
		if (bdd_var(satValuation) != existsDia) {
			// Formula is true in the valuation:
			satValVars.push_back(std::pair<int, bool>(bdd_var(satValuation), true));
		}
		if (bdd_high(satValuation) != bddtrue) {
			// Extract formulae from the rest of the bdd.
			extractAllVars(bdd_high(satValuation), satValVars);
		}
	} else {//bdd_high == bddfalse, by def of satone.
		// ignore the existsDia var
		if (bdd_var(satValuation) != existsDia) {
			// Formula was false in the valuation.
			satValVars.push_back(std::pair<int, bool>(bdd_var(satValuation), false));
		}
		if (bdd_low(satValuation) != bddtrue) {
			// Extract formulae from the rest of the bdd.
			extractAllVars(bdd_low(satValuation), satValVars);
		}
	}
}


/*
 *	Get the 'leftmost' satisfying valuation from the given bdd.
 *
 *	Custom alternative to bdd_satone(bdd). This function ought to be identical to it.
 *	NOTE: While this is functionally identical, it is significantly less
 *	efficient than the BuDDy function bdd_satone(). Should only use this to compare
 *	results with rightVal, otherwise bdd_satone() is preferred.
 */
bdd leftValuation(bdd b) {
	bdd val = bddtrue;
	while (b != bddtrue) {
		if (bdd_low(b) != bddfalse) {
			val = val & bdd_nithvar(bdd_var(b));
			b = bdd_low(b);
		} else {
			val = val & bdd_ithvar(bdd_var(b));
			b = bdd_high(b);
		}
	}
	return val;
}

/*
 *	Get the 'rightmost' satisfying valuation from the given bdd.
 *
 *	Custom alternative to bdd_satone(bdd).
 */
bdd rightValuation(bdd b) {
	bdd val = bddtrue;
	while (b != bddtrue) {
		if (bdd_high(b) != bddfalse) {
			val = val & bdd_ithvar(bdd_var(b));
			b = bdd_high(b);
		} else {
			val = val & bdd_nithvar(bdd_var(b));
			b = bdd_low(b);
		}
	}
	return val;
}

/*
 *	Performs the standard unboxing of a box variable.
 *	
 *	Utilises a cache of previous unboxings.
 *	Cache is currently unbounded in size. Everything will be cached the
 *	first time it is unboxed.
 *	
 *	Assumes 'var' is a box variable.
 */
bdd unbox(int var) {
	if (!unboxed.at(var)) {
		unboxed.at(var) = true;
		//if (S4 || !undiamonded.at(var)) {
			unboxings.at(var) = toBDD(&((varsToAtoms.at(var))->getleft()));
		//} else {
		//	unboxings.at(var) = !undiamondings.at(var);
		//}
		// Statistics:
		++cachedUnboxings;
		--unboxCacheHits;
	}
	// Statistics:
	++unboxCacheHits;
	return unboxings.at(var);
}

/*
 *	Performs the standard undiamonding of a diamond variable.
 *	
 *	Utilises a cache of previous undiamondings.
 *	Cache is currently unbounded in size. Everything will be cached the
 *	first time it is undiamonded.
 *	
 *	Assumes 'var' is a diamond variable (stored in BoxNNF as []~phi).
 *	
 *	For K, can check for the same var unboxed, and take it's negation.
 */
bdd undiamond(int var) {
	if (!undiamonded.at(var)) {
		undiamonded.at(var) = true;
		//if (S4 || !unboxed.at(var)) {
			undiamondings.at(var) = toNotBDD(&((varsToAtoms.at(var))->getleft()));
		//} else {
		//	undiamondings.at(var) = !unboxings.at(var);
		//}
		// Statistics:
		++cachedUndiamondings;
		--undiamondCacheHits;
	}
	// Statistics:
	++undiamondCacheHits;
	return undiamondings.at(var);
}

/*
 *	Performs greedy unboxing in S4. Box subformulae are also unboxed if
 *	they are only conjunctions away from their parent box.
 *	
 *	Utilises a cache of previous unboxings.
 *	Cache is currently unbounded in size. Everything will be cached the
 *	first time it is unboxed.
 *	
 *	Assumes 'var' is a box variable.
 */
bdd unboxS4(int var) {
	if (!unboxed.at(var)) {
		unboxed.at(var) = true;
		unboxings.at(var) = toBDDS4Unbox(&((varsToAtoms.at(var))->getleft()));
		// Statistics:
		++cachedUnboxings;
		--unboxCacheHits;
	}
	// Statistics:
	++unboxCacheHits;
	return unboxings.at(var);
}






































