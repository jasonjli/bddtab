#ifndef _KFORMULA_H_
#define _KFORMULA_H_

#include <set>
#include <string>
#include <memory>
#include <vector>

#include <ostream>

class KFormula {
public:

  enum KFormulaType {
	TRUE, FALSE, 
	AP,                // Atomic Proposition
	DIA, BOX,          // Modal operators
	NOT,               // Unary classical operator
	IMP, EQU, AND, OR  // Binary classical operators
  };
  static const unsigned int TYPE_COUNT = OR+1;

  KFormula(KFormulaType, KFormula* , KFormula*);
  KFormula(KFormulaType, KFormula* , std::shared_ptr<KFormula> const &);
  KFormula(KFormulaType, std::shared_ptr<KFormula> const &, KFormula*);
  KFormula(KFormulaType, std::shared_ptr<KFormula> const &, std::shared_ptr<KFormula> const &);
  KFormula(const std::string&);
  explicit KFormula(bool);

  KFormula(const KFormula & other);
  
  ~KFormula();

  bool operator==(const KFormula&) const;

  size_t size() const;


  std::string* toString() const;

  int compare(const KFormula&) const;

  KFormulaType getop() const {return op;};
  KFormula& getleft() const {return *left;};
  const std::shared_ptr<KFormula> getleftptr() const {return left;};
  KFormula& getright() const {return *right;};
  const std::shared_ptr<KFormula> getrightptr() const {return right;};
  void setvar(int _var) { var = _var; };
  int getvar() const { return var; };
  void setrole(int _role) {role = _role;};
  int getrole() const {return role;};
  void setprop(std::string _prop) { prop = _prop; };
  std::string getprop() { return prop; };

  static KFormula* parseKFormula(const char*);

  struct equal_to {bool operator()(const KFormula& a, const KFormula& b) const {return a == b;}};
  struct less_than {bool operator()(const KFormula& a, const KFormula& b) const {return a.compare(b) > 0;}};
  struct greater_than {bool operator()(const KFormula& a, const KFormula& b) const {return a.compare(b) < 0;}};
  


  friend std::ostream& operator<<(std::ostream& ,const KFormula& );

private:
  KFormula();

  void toString(std::string& buf, int precedence) const;

  static KFormula* parseEQU(const char*& str);
  static KFormula* parseIMP(const char*& str);
  static KFormula* parseAND(const char*& str);
  static KFormula* parseOR(const char*& str);
  static KFormula* parseRest(const char*& str);

  KFormulaType op;
  std::shared_ptr<KFormula> left, right;
  std::string prop;
  int var;
  int role;

};




#endif
