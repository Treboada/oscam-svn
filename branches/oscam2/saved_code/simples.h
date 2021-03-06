/*
 * simples.h
 *
 *      Author: aston
 */

#ifndef SIMPLES_H_
#define SIMPLES_H_

#include "global.h"

//--------------------------------------------------------------------
// CLASS --- simples ---
//--------------------------------------------------------------------
class t_simples
{
private:
	 int gethexval(char c);
	 int inet_byteorder;
public:
	 t_simples();
	~t_simples();

	 void  trim(string *str);
	 char* trim(char   *str);
	 void  strToLower(string *str);
	 int   key_atob(char *asc, uchar *bin);
	 in_addr_t cs_inet_addr(char *str);
	 in_addr_t cs_inet_order(in_addr_t n);
	 ulong a2i(char *asc, int bytes);
	 int   key_atob14(char *asc, uchar *bin);
	 int   cs_atob(uchar *buf, char *asc, int n);
	 string itoa(int value, int base);
	 string cs_hexdump(int m, uchar *buf, int n);
};
//---------------------------------------------------------------------------

#endif /* SIMPLES_H_ */
