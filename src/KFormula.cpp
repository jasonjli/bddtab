#include "KFormula.h"
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <algorithm>

#include <iostream>

KFormula::KFormula()
  :op(TRUE)
  ,left()
  ,right()
  ,prop("")
{

}

KFormula::KFormula(const std::string& s)
  :op(AP)
  ,left()
  ,right()
  ,prop(s)
{
}

KFormula::KFormula(bool b)
  :op(b ? TRUE : FALSE)
  ,left()
  ,right()
  ,prop("")
{
}

KFormula::KFormula(KFormulaType _op, KFormula * _left, KFormula * _right)
  :op(_op)
  ,left(_left)
  ,right(_right)
  ,prop("")
{
}

KFormula::KFormula(KFormulaType _op, KFormula* _left, std::shared_ptr<KFormula> const & _right)
  :op(_op)
  ,left(_left)
  ,right(_right)
  ,prop("")
{
}
KFormula::KFormula(KFormulaType _op, std::shared_ptr<KFormula> const & _left, KFormula* _right)
  :op(_op)
  ,left(_left)
  ,right(_right)
  ,prop("")
{
}
KFormula::KFormula(KFormulaType _op, std::shared_ptr<KFormula> const & _left, std::shared_ptr<KFormula> const & _right)
  :op(_op)
  ,left(_left)
  ,right(_right)
  ,prop("")
{
}


KFormula::KFormula(const KFormula & other)
  :op(other.op)
  ,left(other.left)
  ,right(other.right)
  ,prop(other.prop)
{  
}


KFormula::~KFormula() {
}

bool KFormula::operator==(const KFormula& other) const {
  return 
	op == other.op &&
	(op <= FALSE ? true :                               //Constants
	 op == AP    ? prop == other.prop :                 //Propositions
	 op <= NOT   ? *left == *(other.left) :             //Unary operators
	 *left == *(other.left) && *right == *(other.right) //Binary operators
	 );
}





int KFormula::compare(const KFormula & other) const {
  if (op != other.op) return static_cast<int>(op - other.op);
  switch(op) {
  case TRUE:
  case FALSE:
	return 0;
  case AP:
	return prop.compare(other.prop);
  case DIA:
  case BOX:
  if (prop.compare(other.prop) != 0) return prop.compare(other.prop);
  case NOT:
	return left->compare(*other.left);
  case OR:
  case AND:
  case EQU:
  case IMP:
	int c1 = left->compare(*other.left);
	if (c1 != 0) return c1;
	else return right->compare(*other.right);
  }
  assert(false && "Fell out of complete switch!");
}


size_t KFormula::size() const {
  switch(op) {
  case TRUE:
  case FALSE:
  case AP: return 1;
  case BOX:
  case DIA:
  case NOT:
	return 1 + left->size();
  case IMP:
  case EQU: 
  case AND:
  case OR: return 1 + left->size() + right->size();
  default:
	// Error!
	assert(false);
	return 0;
  }
}



std::string* KFormula::toString() const {
  std::string *ret = new std::string;
  toString(*ret,-1);
  return ret;
}

static int prionr(KFormula::KFormulaType op) {
  switch(op) {
  case KFormula::EQU: return 0;
  case KFormula::IMP: return 2;
  case KFormula::OR: return 3;
  case KFormula::AND: return 4;
  default: return 5;
  }
}

void KFormula::toString(std::string& s, int precedence) const {
  bool needsBrackets = prionr(op) < precedence;
  if (needsBrackets)
	s += '(';
  switch(op){
  case TRUE: s+="True "; break;
  case FALSE: s+="False "; break;
  case AP: s+= prop; break;
  case BOX:
	s += " [" + prop + "] ";
	left->toString(s,5);
	break;
  case DIA:
	s += " <" + prop + "> ";
	left->toString(s,5);
	break;
  case NOT:
	s += " ~ ";
	left->toString(s,5);
	break;
  case EQU:
	left->toString(s,0);
	s += " <=> ";
	right->toString(s,0);
	break;
  case IMP:
	left->toString(s,3);
	s += " => ";
	right->toString(s,3);
	break;
  case OR:
	left->toString(s,4);
	s += " | ";
	right->toString(s,4);
	break;
  case AND:
	left->toString(s,5);
	s += " & ";
	right->toString(s,5);
	break;
  default:
	assert(false);
	// Shouldn't get here!
  }

  if (needsBrackets)
	s += ')';

  return;
}



std::ostream& operator<<( std::ostream& stream, const KFormula& f ){
  std::unique_ptr<std::string> s(f.toString());
  return stream << *s;
}


KFormula*  KFormula::parseEQU(const char*& str) {
  KFormula* left = parseIMP(str);
  
  while(isspace(*str)) ++str;
  
  if(strncmp(str,"<=>",3) == 0) {
	// This is an equivalence
	str += 3;
	KFormula* right = parseEQU(str);
	return new KFormula(KFormula::EQU,left,right);
  } else {
	return left;
	// This is not an equivalence
  }
}

KFormula* KFormula::parseIMP(const char*& str) {
  KFormula* left = parseOR(str);
  
  while(isspace(*str)) ++str;
  
  if(strncmp(str,"=>",2) == 0) {
	// This is an implication
	str += 2;
	KFormula* right = parseIMP(str);
	return new KFormula(KFormula::IMP,left,right);
  } else {
	return left;
	// This is not an implication
  }
}

KFormula* KFormula::parseOR(const char*& str) {
  KFormula* left = parseAND(str);
  
  while(isspace(*str)) ++str;
  
  if(*str == '|'){
	// This is a disjunction
	++str;
	KFormula* right = parseOR(str);
	return new KFormula(KFormula::OR,left,right);
  } else {
	return left;
	// This is not a disjunction
  }
}

KFormula* KFormula::parseAND(const char*& str) {
  KFormula* left = parseRest(str);
  
  while(isspace(*str)) ++str;
  
  if(*str == '&'){
	// This is a conjunction
	++str;
	KFormula* right = parseAND(str);
	return new KFormula(KFormula::AND,left,right);
  } else {
	return left;
	// This is not a conjunction
  }
}

KFormula* KFormula::parseRest(const char*& str) {
  while(isspace(*str)) ++str;


  KFormula *left;

  if(*str == '('){
	++str;
	left = parseEQU(str);
	while(isspace(*str)) ++str;
	assert(*str == ')');
	++str;
	return left;
  } else if (strncmp(str,"<", 1) == 0 && (isalnum(*(str + 1)) || *(str+1) == '-')) {
  size_t n = 1;// "<"
  if (*(str+1) == '-') ++n;
  while(isalnum(*(str+n))) ++n;
  std::string roleString(str+1, n - 1);
  ++n;// ">"
	str += n;
	left = parseRest(str);
	left = new KFormula(KFormula::DIA,left,NULL);
	left->setprop(roleString);
	return left;
  } else if (strncmp(str,"<>", 2) == 0) {
	str += 2;
	left = parseRest(str);
	left = new KFormula(KFormula::DIA,left,NULL);
	return left;
  } else if (strncmp(str,"[", 1) == 0 && (isalnum(*(str + 1)) || *(str+1) == '-')) {
  size_t n = 1;// "["
  if (*(str+1) == '-') ++n;
  while(isalnum(*(str+n))) ++n;
  std::string roleString(str+1, n - 1);
  ++n;// "]"
	str += n;
	left = parseRest(str);
	left = new KFormula(KFormula::BOX,left,NULL);
	left->setprop(roleString);
	return left;
  }else if (strncmp(str,"[]",2) == 0) {
	str += 2;
	left = parseRest(str);
	left = new KFormula(KFormula::BOX,left,NULL);
	return left;
  } else if (*str=='~') {
	++str;
	left = parseRest(str);
	left = new KFormula(KFormula::NOT,left,NULL);
	return left;
  } else if (strncmp(str,"True",4)==0) {
	str += 4;
	return new KFormula(true);
  } else if (strncmp(str,"False",5)==0) {
	str += 5;
	return new KFormula(false);
  } else if (isalpha(*str) || *str == '_') {
	size_t n = 1;
	while(isalnum(*(str+n)) || *(str+n) == '_') ++n;
	left = new KFormula(std::string(str,n));
	str += n;
	return left;
  }

  assert(false && "Could not parse input");
  // Must not reach here

}

KFormula* KFormula::parseKFormula(const char* str) {
  if (!str || !*str) return NULL;

  KFormula* ret = parseEQU(str);

  while(isspace(*str)) ++str;

  if (*str) { std::cerr << "ERROR: \""<< *str << "\"" << std::endl;}
  assert(! *str);

  return ret;
}

