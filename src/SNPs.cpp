/*
 * SNPs.cpp
 *
 *  Created on: Jan 14, 2013
 *      Author: pickrell
 */

#include "SNPs.h"
using namespace std;

SNPs::SNPs(){

}

SNPs::SNPs(Fgwas_params *p){
	params = p;
	params->print_stdout();
	for (vector<string>::iterator it = params->distmodels.begin(); it != params->distmodels.end(); it++) dmodels.push_back( read_dmodel(*it));
	//cout << "reading from "<< infile << "\n"; cout.flush();
	if (params->zformat) load_snps_z(params->infile, params->V, params->wannot, params->dannot, params->segannot);
	else {
		cerr << "ERROR: need z-score format for now\n";
		exit(1);
		//load_snps(infile, prior, annot);
	}
	if (params->finemap) make_segments_finemap();
	else{
		make_chrsegments();
		make_segments(params->K);
	}
	check_input();
	snppri.clear();
	segpi = 0.001;
	init_segpriors();
    phi = (1+sqrt(5))/2;
    resphi = 2-phi;
	for (int i = 0; i < d.size(); i++){
		snppri.push_back(1.0);
		snppost.push_back(0.0);
	}
	nannot = annotnames.size();
	nsegannot = segannotnames.size();
	for (int i = 0; i < nannot; i++)	lambdas.push_back(0);
	for (int i = 0; i < nsegannot; i++) seglambdas.push_back(0);
	set_priors();
}

void SNPs::check_input(){
	int totalsize = 0;
	for (vector<pair<int, int> >::iterator it = segments.begin(); it != segments.end(); it++){

		int st = it->first;
		int sp = it->second;
		totalsize += (d[sp-1].pos-d[st].pos);
		int prevpos = d[st].pos;
		string prevchr = d[st].chr;
		for (int i= st+1; i < sp; i++){
			string testchr = d[i].chr;
			int testpos = d[i].pos;
			if (testchr == prevchr and prevpos > testpos){
				cerr<< "ERROR: SNPs out of order\nChromosome "<<testchr << ". Position "<< prevpos << " seen before "<< testpos<< "\n";
				exit(1);
			}
			prevpos = testpos;
			prevchr = testchr;
		}
	}
	float meansize = (float) totalsize /  (float) segments.size();
	meansize = meansize/1000000.0;
	cout << "Mean segment size: "<< meansize << " Mb\n";
	if (meansize > 10.0){
		cout << "\n****\n**** WARNING: mean segment size is over 10Mb, this often causes convergence problems (in human data). Consider reducing window size (using -k).\n****\n\n"; cout.flush();
	}
}
vector<pair<int, int> > SNPs::read_dmodel(string infile){
	vector<pair<int, int> > toreturn;
	ifstream in(infile.c_str());
	vector<string> line;
	struct stat stFileInfo;
	int intStat;
	string st, buf;
	//cout << "reading " << infile << "\n";
	intStat = stat(infile.c_str(), &stFileInfo);
	if (intStat !=0){
		std::cerr<< "ERROR: cannot open file " << infile << "\n";
		exit(1);
	}
    while(getline(in, st)){
    	buf.clear();
    	stringstream ss(st);
    	line.clear();
    	while (ss>> buf){
    		line.push_back(buf);
    	}
    	int start = atoi(line[0].c_str());
    	int stop = atoi(line[1].c_str());
    	if (stop < start){
    		cerr <<"ERROR: in distance model "<< infile<< " , " << start << " is after "<< stop << "\n";
    		exit(1);
    	}
    	toreturn.push_back(make_pair(start, stop));
    }
	return toreturn;
}

void SNPs::init_segpriors(){
	segannot.clear();
	segpriors.clear();
	segannotnames.clear();
	vector<double> segmeans;
	vector<double> means2sort;
	for (vector<pair<int, int> >::iterator it = segments.begin(); it != segments.end(); it++){
		segpriors.push_back(segpi);
		if (params->segannot.size() > 0){
			double segmean = 0.0;
			int total = 0;
			for (int i = it->first; i < it->second; i++){
				segmean += d[i].dens;
				total ++;
			}
			segmeans.push_back( segmean / (double) total);
			means2sort.push_back( segmean / (double) total);
		}
	}
	if (params->segannot.size() < 1) return;

	segannotnames.push_back(params->segannot[0] +"_lo");
	segannotnames.push_back(params->segannot[0] +"_hi");
	sort(means2sort.begin(), means2sort.end());
	double locutoff = means2sort[floor( (double) means2sort.size()* params->loquant ) ];
	double hicutoff = means2sort[floor( (double) means2sort.size()* params->hiquant ) ];
	//cout << locutoff << " "<< hicutoff << "\n"; cout.flush();
	for (int i = 0; i < segments.size(); i++){
		vector<bool> annots;
		double m = segmeans[i];
		if (m <= locutoff) annots.push_back(true);
		else annots.push_back(false);
		if (m >= hicutoff) annots.push_back(true);
		else annots.push_back(false);
		segannot.push_back(annots);
	}
}

void SNPs::load_snps_z(string infile, double prior, vector<string> annot, vector<string> dannot, vector<string> segannot){
	igzstream in(infile.c_str()); //only gzipped files
	vector<string> line;
	struct stat stFileInfo;
	int intStat;
	string st, buf;

	intStat = stat(infile.c_str(), &stFileInfo);
	if (intStat !=0){
		std::cerr<< "ERROR: cannot open file " << infile << "\n";
		exit(1);
	}

	// read header
	getline(in, st);
	buf.clear();
	stringstream ss(st);
	line.clear();
	while (ss>> buf){
		line.push_back(buf);
	}

	//make a map of header to index
	map<string, int> header_index;
	for (int i = 0; i < line.size(); i++){
		header_index[line[i]] = i;
	}
	// get the indices of the annotations
	vector<int> annot_index;
   	for (vector<string>::iterator it = annot.begin(); it != annot.end(); it++){
   		int i = 0;
   		bool found = false;
   		while (i < line.size() and !found){
   			if (line[i] == *it) {
   				annot_index.push_back(i);
   				found = true;
   			}
   			i++;
   		}
   		if (!found){
   			cerr << "ERROR: cannot find annotation "<< *it << "\n";
   			exit(1);
   		}
    	annotnames.push_back(*it);
   	}

   	// get indices of distance annotations
   	vector<int> dannot_index;
   	for (int j = 0; j < dannot.size(); j++){

   		string jname = dannot[j];
   		int i = 0;
   		bool found = false;
   		while (i < line.size() and !found){
   			if (line[i] == jname) {
   				dannot_index.push_back(i);
   				found = true;
   			}
   			i++;
   		}
   		if (!found){
   			cerr << "ERROR: cannot find distance annotation "<< jname << "\n";
   			exit(1);
   		}
   		append_dannotnames(jname, dmodels[j]);
   	}
   	// get indices for segannot
   	int segannotindex;
   	if (segannot.size() > 0){
   		if (header_index.find(segannot[0]) == header_index.end()){
   			cerr << "ERROR: cannot find segment annotation "<< segannot[0] << "\n";
   			exit(1);
   		}
   		segannotindex = header_index[segannot[0]];
   	}
   	// get indices for the rs, maf, chr, pos, N, Ncase, Ncontrol,
   	int rsindex, mafindex, chrindex, zindex, posindex, Nindex, Ncaseindex, Ncontrolindex, segnumberindex, condindex;
   	bool override_v = false;
   	int seindex;
   	if (header_index.find("SE") != header_index.end()){
   		cout << "WARNING: detected SE in header, will override F and N\n";
   		seindex = header_index["SE"];
   		override_v = true;
   	}
   	if (header_index.find("SNPID") == header_index.end()){
   		cerr << "ERROR: cannot find SNPID in header\n";
   		exit(1);
   	}
   	else rsindex = header_index["SNPID"];

  	if (header_index.find("F") == header_index.end()){
   		cerr << "ERROR: cannot find F in header\n";
   		exit(1);
   	}
   	else mafindex = header_index["F"];

 	if (header_index.find("CHR") == header_index.end()){
   		cerr << "ERROR: cannot find CHR in header\n";
   		exit(1);
   	}
   	else chrindex = header_index["CHR"];

 	if (header_index.find("POS") == header_index.end()){
   		cerr << "ERROR: cannot find POS in header\n";
   		exit(1);
   	}
   	else posindex = header_index["POS"];

	if (header_index.find("Z") == header_index.end()){
   		cerr << "ERROR: cannot find Z in header\n";
   		exit(1);
   	}
   	else zindex = header_index["Z"];

	if (header_index.find("N") == header_index.end() && !params->cc){
   		cerr << "ERROR: cannot find N in header\n";
   		exit(1);
   	}
   	else Nindex = header_index["N"];

	if (header_index.find("NCASE") == header_index.end() && params->cc){
   		cerr << "ERROR: cannot find NCASE in header\n";
   		exit(1);
   	}
   	else Ncaseindex = header_index["NCASE"];

	if (header_index.find("NCONTROL") == header_index.end() && params->cc){
   		cerr << "ERROR: cannot find NCONTROL in header\n";
   		exit(1);
   	}
   	else Ncontrolindex = header_index["NCONTROL"];

	if (header_index.find("SEGNUMBER") == header_index.end() && params->finemap){
   		cerr << "ERROR: cannot find SEGNUMBER in header\n";
   		exit(1);
   	}
   	else segnumberindex = header_index["SEGNUMBER"];

	if (params->cond && header_index.find(params->testcond_annot) == header_index.end()){
		cerr << "ERROR: cannot find annotation "<< params->testcond_annot << "\n";
		exit(1);
	}
	else if (params->cond) condindex = header_index[params->testcond_annot];
	string oldchr = "NA";
    while(getline(in, st)){
    	buf.clear();
    	stringstream ss(st);
    	line.clear();
    	while (ss>> buf){
    		line.push_back(buf);
    	}
    	string rs = line[rsindex];

    	double alfreq = atof(line[mafindex].c_str());
    	if (alfreq < 1e-8) continue;
    	double z = atof(line[zindex].c_str());
    	if (!params->cc){
    		int N = atoi(line[Nindex].c_str());
    		string chr = line[chrindex];
    		if (chr != oldchr) {
    			cout << "Reading "<< chr << "\n"; cout.flush();
    			oldchr = chr;
    		}
    		if (params->dropchr and chr == params->chrtodrop) continue;
    		int pos = atoi(line[posindex].c_str());
    		vector<bool> an;
    		vector<int> dists;
    		for (vector<int>::iterator it = annot_index.begin(); it != annot_index.end(); it++){
    			if (line[*it] == "1") an.push_back(true);
    			else if (line[*it] == "0") an.push_back(false);
    			else{
    				cerr << "ERROR: only 0 and 1 allowed for annotations, found "<< line[*it] <<"\n";
    				exit(1);
    			}
    		}
    		for (vector<int>::iterator it = dannot_index.begin(); it != dannot_index.end(); it++){
    			dists.push_back( atoi(line[*it].c_str()));
    		}

    		SNP s(rs, chr , pos, N, alfreq, z, prior, an, dists, dmodels);
    		if (params->finemap){
    			int snumber = atoi(line[segnumberindex].c_str());
    			s.chunknumber = snumber;
    		}
    		if (segannot.size() > 0) s.dens = atof(line[segannotindex].c_str());
    		if (override_v){
    			float se = atof(line[seindex].c_str());
    			s.V = se*se;
    			s.BF = s.calc_logBF();
    		}
    		if (params->cond){
    			if (line[condindex] == "1") s.condannot =true;
    			else if (line[condindex] == "0") s.condannot = false;
    			else{
    				cerr << "ERROR: only 0 and 1 allowed for annotations, found "<< line[condindex] <<"\n";
    				exit(1);
    			}
    		}
    		d.push_back(s);
    	}
    	else{
    		//cout << "reading "<< rs << "\n"; cout.flush();
      		int Ncase = atoi(line[Ncaseindex].c_str());
      		//cout << "Ncase: "<< Ncase << " "<< Ncaseindex << "\n"; cout.flush();
      		int Ncontrol = atoi(line[Ncontrolindex].c_str());
      		string chr = line[chrindex];
      		if (params->dropchr and chr == params->chrtodrop) continue;
      		int pos = atoi(line[posindex].c_str());
      		vector<bool> an;
      		vector<int> dists;
      		for (vector<int>::iterator it = annot_index.begin(); it != annot_index.end(); it++){
    			if (line[*it] == "1") an.push_back(true);
    			else if (line[*it] == "0") an.push_back(false);
    			else{
    				cerr << "ERROR: only 0 and 1 allowed for annotations, found "<< line[*it] <<"\n";
    				exit(1);
    			}

        	}
      		for (vector<int>::iterator it = dannot_index.begin(); it != dannot_index.end(); it++){
      			dists.push_back( atoi(line[*it].c_str()));
        	}

      		SNP s(rs, chr , pos, Ncase, Ncontrol, alfreq, z, prior, an, dists, dmodels);
    		if (params->finemap){
    			int snumber = atoi(line[segnumberindex].c_str());
    			s.chunknumber = snumber;
    		}
      		if (segannot.size() > 0) s.dens = atof(line[segannotindex].c_str());
       		if (override_v){
       			float se = atof(line[seindex].c_str());
       			s.V = se*se;
       			s.BF = s.calc_logBF();
       			//cout << s.id << " "<< s.V << " "<< s.BF << " "<< s.Z << " "<< s.f<< "\n";
       		}
      		if (params->cond){
      			if (line[condindex] == "1") s.condannot =true;
      			else if (line[condindex] == "0") s.condannot = false;
      			else{
      				cerr << "ERROR: only 0 and 1 allowed for annotations, found "<< line[condindex] <<"\n";
      				exit(1);
      			}
      		}
      		d.push_back(s);

    	}
    }
}

void SNPs::append_dannotnames(string name, vector<pair<int, int> > model){
	for (vector<pair<int, int> >::iterator it = model.begin(); it != model.end(); it++){
		stringstream ss;
		ss << name << "_" << it->first << "_"<< it->second;
		annotnames.push_back(ss.str());
	}
}

vector<pair< pair<int, int>, pair<double, double> > > SNPs::get_cis(){
	vector<pair<pair<int, int>, pair<double, double> > > toreturn;
	double startsegpi = segpi;
	vector<double> startlambdas;
	vector<double> startseglambdas;
	for (vector<double>::iterator it = lambdas.begin(); it != lambdas.end(); it++) startlambdas.push_back(*it);
	for (vector<double>::iterator it = seglambdas.begin(); it != seglambdas.end(); it++) startseglambdas.push_back(*it);
	if (!params->finemap) {
		toreturn.push_back(get_cis_segpi());
		segpi = startsegpi;
		set_priors();
		for (int i = 0; i < seglambdas.size(); i++){
			toreturn.push_back(get_cis_seglambda(i));
			seglambdas[i] = startseglambdas[i];
			set_priors();
		}
	}
	segpi = startsegpi;
	for (int i = 0; i < lambdas.size(); i++){
		toreturn.push_back(get_cis_lambda(i));
		lambdas[i] = startlambdas[i];
		set_priors();
	}
	return toreturn;
}



pair< pair<int, int>, pair<double, double> > SNPs::get_cis_condlambda(){
	pair< pair<int, int>, pair<double, double> > toreturn;
	double startlk = llk();
	double thold = startlk - 2;
	cout <<  startlk << " "<< thold << "\n";
	double min = -20.0;
	double max = 20.0;
	double test = condlambda;
	if (test > max) max = test+20.0;
	if (test < min) min = test-20.0;
	if (max < 0) max = 20.0;
	if (min > 0) min = -20.0;
	double start = (test+max)/2;
	double tau = 0.001;
	int convhi = golden_section_condlambda_ci(test, start, max, tau, thold);
	double hi = condlambda;
	cout << hi << " "<< llk() << " hi\n";
	start = (test+min)/2;
	int convlo = golden_section_condlambda_ci(min, start, test, tau, thold);
	double lo = condlambda;
	cout << lo << " "<< llk() << " lo\n";
	pair<int, int> conv = make_pair(convlo, convhi);
	pair<double, double> ci = make_pair(lo, hi);
	return make_pair(conv, ci);
}

pair< pair<int, int>, pair<double, double> > SNPs::get_cis_segpi(){
	pair< pair<int, int>, pair<double, double> > toreturn;
	double startsegpi = segpi;
	double startlk = llk();
	double thold = startlk - 2;
	//cout <<  startlk << " "<< thold << "\n";
	double test =  log(startsegpi) - log(1-startsegpi);

	double min = -10.0;
	double max = 10.0;
	int nit = 0;
	double start = (test+max)/2;
	double tau = 0.001;
	int convhi = golden_section_segpi_ci(test, start, max, tau, thold,  &nit);
	double hi = segpi;
	cout << hi << " "<< llk() << " hi\n";
	start = (test+min)/2;
	nit = 0;
	int convlo = golden_section_segpi_ci(min, start, test, tau, thold, &nit);
	double lo = segpi;
	cout << lo << " "<< llk() << " lo\n";
	pair<int, int> conv = make_pair(convlo, convhi);
	pair<double, double> ci = make_pair(lo, hi);
	return make_pair(conv, ci);
}

pair< pair<int, int>, pair<double, double> > SNPs::get_cis_lambda(int which){
	pair< pair<int, int>, pair<double, double> > toreturn;

	double startlk = llk();
	double thold = startlk - 2;
	cout <<  startlk << " "<< thold << "\n";
	double min = -20.0;
	double max = 20.0;
	double test = lambdas[which];
	if (test > max) max = test+20.0;
	if (test < min) min = test-20.0;
	if (max < 0) max = 20.0;
	if (min > 0) min = -20.0;
	double start = (test+max)/2;
	double tau = 0.001;
	int convhi = golden_section_lambda_ci(test, start, max, tau, which, thold);
	double hi = lambdas[which];
	cout << hi << " "<< llk() << " hi\n";
	start = (test+min)/2;
	int convlo = golden_section_lambda_ci(min, start, test, tau, which, thold);
	double lo = lambdas[which];
	cout << lo << " "<< llk() << " lo\n";
	pair<int, int> conv = make_pair(convlo, convhi);
	pair<double, double> ci = make_pair(lo, hi);
	return make_pair(conv, ci);
}


pair< pair<int, int>, pair<double, double> > SNPs::get_cis_seglambda(int which){
	pair< pair<int, int>, pair<double, double> > toreturn;

	double startlk = llk();
	double thold = startlk - 2;
	cout <<  startlk << " "<< thold << "\n";
	double min = -20.0;
	double max = 20.0;
	double test = seglambdas[which];
	if (test > max) max = test+20.0;
	if (test < min) min = test-20.0;
	if (max < 0) max = 20.0;
	if (min > 0) min = -20.0;
	double start = (test+max)/2;
	double tau = 0.001;
	int convhi = golden_section_seglambda_ci(test, start, max, tau, which, thold);
	double hi = seglambdas[which];
	cout << hi << " "<< llk() << " hi\n";
	start = (test+min)/2;
	int convlo = golden_section_seglambda_ci(min, start, test, tau, which, thold);
	double lo = seglambdas[which];
	cout << lo << " "<< llk() << " lo\n";
	pair<int, int> conv = make_pair(convlo, convhi);
	pair<double, double> ci = make_pair(lo, hi);
	return make_pair(conv, ci);
}

void SNPs::print(){
	cout << "rs chr pos BF Z";
	for (int i =0; i < nannot; i++) cout << " "<< annotnames[i];
	cout << "\n";
	for (vector<SNP>::iterator it = d.begin(); it != d.end(); it++){
		cout << it->id << " "<< it->chr << " "<< it->pos << " "<< it->BF <<  " "<< it->Z;
		for (int i = 0; i < nannot; i++) cout << " "<< it->annot[i];
		cout << "\n";
	}
}

void SNPs::print(string outfile, string outfile2){
	ogzstream out(outfile.c_str());
	ogzstream out2(outfile2.c_str());
	out << "id chr pos logBF Z V pi pseudologPO pseudoPPA PPA chunk";
	out2 << "chunk chr st sp max_abs_Z logBF pi logPO PPA";
	for (vector<string>::iterator it = annotnames.begin(); it != annotnames.end(); it++) out << " "<< *it;
	out << "\n";
	for (vector<string>::iterator it = segannotnames.begin(); it != segannotnames.end(); it++) out2 << " "<< *it;
	out2 << "\n";
	int segnum = 0;
	for (vector<pair<int, int> >::iterator it = segments.begin(); it != segments.end(); it++){
		int stindex = it->first;
		int spindex = it->second;
		out2 << segnum << " "<< d[stindex].chr << " "<< d[stindex].pos << " "<< d[spindex].pos << " ";
		double segp = segpriors[segnum];
		double seglpio = log(segp)- log(1-segp);
		double seglPO;
		double segbf = 0;
		double segPPA;
		double sum = 0;
		double maxZ = 0;
		for (int i = stindex; i < spindex; i++){
			double pi = exp(snppri[i]);
			double bf = exp(d[i].BF);
			double Z = fabs(d[i].Z);
			if (Z> maxZ) maxZ = Z;
			segbf+= pi*bf;
		}
		seglPO = log(segbf)+ seglpio;
		segPPA = exp(seglPO)/ (1+ exp(seglPO));
		out2 << maxZ<< " "<< log(segbf) << " " << segp << " "<< seglPO << " "<< segPPA;
		for (int i = 0; i < nsegannot; i++) out2 << " "<< segannot[segnum][i];
		out2 << "\n";
		for (int i =stindex ; i < spindex; i++){
			//double pi = snppri[i]*segpi;
			double pi = exp(snppri[i])*segp;
			double num = exp(snppri[i])*exp(d[i].BF);
			double lpio = log(pi) - log(1-pi);
			double cPPA = num/segbf;
			double lPO = d[i].BF + lpio;
			double tPPA = cPPA*segPPA;
			double PPA = exp(lPO)/  ( 1+ exp(lPO));
			out << d[i].id << " "<< d[i].chr << " "<< d[i].pos << " "<< d[i].BF <<  " "<< d[i].Z <<  " " << d[i].V << " "<< snppri[i] << " "<< lPO  << " "<< PPA << " " << tPPA << " "<< segnum;
			for (int j = 0; j < annotnames.size(); j++) out << " "<< d[i].annot[j];
			out << "\n";
		}
		segnum++;
	}
}

double SNPs::cross10(bool penalize){
	//do 10-fold cross validation
	//
	// split segments into groups
	// L = 1/10* \sum_i L*(i)
	// where L*(i) is the likelihood of data in group i after optimizing model without it
	//
	double toreturn =0;
	vector< set<int> > split10 = make_cross10();
	vector<double> Lstar;
	for (vector<set<int> >::iterator it = split10.begin(); it != split10.end(); it++){
		GSL_xv_optim(*it, penalize);
		double tmpl = 0;
		for (set<int>::iterator it2 = it->begin(); it2 != it->end(); it2++) tmpl += llk(*it2);
		//cout << tmpl << "\n";
		Lstar.push_back(tmpl);
		toreturn += tmpl;
	}
	toreturn = toreturn /10.0;
	return toreturn;
}

vector<set<int> > SNPs::make_cross10(){
	vector<set<int> > toreturn;
	int nper = floor((double) segments.size() / 10.0);
	for (int i = 0; i < 10; i++){
		set<int> tmp;
		for (int j = i*nper; j < i*nper+nper; j++) tmp.insert(j);
		toreturn.push_back(tmp);
	}
	//for (vector<set<int> >::iterator it = toreturn.begin(); it != toreturn.end(); it++){
	//	for (set<int>::iterator it2 = it->begin(); it2 != it->end(); it2++){
	//		cout << *it2 << " ";
	//	}
	//	cout << "\n";
	//}

	return toreturn;

}
void SNPs::make_segments(int size){
	segments.clear();
	int counter = 0;
	for (vector<pair<int, int> >::iterator it = chrsegments.begin(); it != chrsegments.end(); it++){
		int starti = it->first;
		int endi = it->second;
		int length = endi-starti;
		int bestmod = length % size;
		int bestsize = size;
		for (int i = size -20; i < size+20; i++){
			if (i < 1) continue;
			int test = length % i;
			if (test < bestmod){
				bestmod = test;
				bestsize = i;
			}
		}
		int nseg = length/bestsize;
		//cout << starti << " "<< endi << " "<< bestsize << " "<< nseg<< "\n";
		for (int i = 0; i < nseg; i++){
			int sstart = starti+i*bestsize;
			int send = starti+i*bestsize+bestsize;
			if (i > (nseg-2))	send = endi;
			//cout << i << " "<< nseg << " "<< starti << " "<< endi << " "<< sstart << " "<< send << "\n";
			segments.push_back(make_pair(sstart, send));
			for (int i = sstart ; i < send ; i++) d[i].chunknumber = counter;
			counter++;
		}
	}
}


void SNPs::make_segments_finemap(){
	segments.clear();
	int sstart = 0;
	int wseg = d[0].chunknumber;
	for (int i = 1; i < d.size(); i++){
		int testseg = d[i].chunknumber;
		if (testseg < wseg || testseg > wseg) {
			segments.push_back(make_pair(sstart, i));
			sstart = i;
			wseg = testseg;
		}
	}
	segments.push_back(make_pair(sstart, d.size()));
}

void SNPs::make_chrsegments(){
	chrsegments.clear();
	int i = 0;
	int start = i;
	int startpos = d[i].pos;
	string startchr = d[i].chr;
	while (i < d.size()){
		int tmppos = d[i].pos;
		string tmpchr = d[i].chr;
		if (tmpchr != startchr){
			int end = i-1;
			chrsegments.push_back(make_pair(start, end));
			start = i;
			startpos = d[i].pos;
			startchr = d[i].chr;
		}
		i++;
	}
	int end = i-1;
	chrsegments.push_back(make_pair(start, end));
}

void SNPs::print_segments(){
	for (vector<pair<int, int> >::iterator it = segments.begin(); it != segments.end(); it++){
		cout << it->first << " "<< it->second << "\n";
	}

}

void SNPs::print_chrsegments(){
	for (vector<pair<int, int> >::iterator it = chrsegments.begin(); it != chrsegments.end(); it++){
		cout << it->first << " "<< it->second << "\n";
	}

}

void SNPs::set_priors(){

	set_segpriors(); // a bit of computation for nothing if there's no segment annotations, spot for speed improvement if necessary
	for (int i = 0; i < segments.size(); i++) set_priors(i);
}

void SNPs::set_priors_cond(){
	set_segpriors();
	for (int i = 0; i < segments.size(); i++) set_priors_cond(i);
}

void SNPs::set_segpriors(){
	for (int i = 0; i < segments.size(); i++){
		double logitprior = log(segpi) - log(1-segpi);
		for (int j = 0; j < nsegannot; j++){
			if (segannot[i][j]) logitprior += seglambdas[j];
		}
		double prior = 1.0/  (1.0 + exp(-logitprior));
		segpriors[i] = prior;
	}
}


void SNPs::set_priors(int which){
	pair<int, int> seg = segments[which];
	int st = seg.first;
	int sp = seg.second;
	double sumxs = 0;
	for (int i = st; i < sp ;i++) {
		//cout << i << " "<< d[i].get_x(lambdas) << "\n";
		//double tmpx = exp(d[i].get_x(lambdas));
		double tmpx = d[i].get_x(lambdas);
		snppri[i] = tmpx;
		//sumxs += tmpx;
		sumxs = sumlog(sumxs, tmpx);
	}
	for (int i = st; i <  sp ; i++) {
		//snppri[i] = snppri[i]/sumxs;
		//doing this in log space
		snppri[i] = snppri[i] - sumxs;
		if (!isfinite(snppri[i])){
			cerr << "ERROR: prior for SNP "<< i << " is " << snppri[i] << "\n";
			exit(1);
		}
		//if (snppri[i] < DBL_MIN) snppri[i] = DBL_MIN;
		//cout << i << " "<< snppri[i] << "\n";
	}
}

void SNPs::set_priors_cond(int which){
	pair<int, int> seg = segments[which];
	int st = seg.first;
	int sp = seg.second;
	double sumxs = 0;
	for (int i = st; i < sp ;i++) {
		//cout << i << " "<< d[i].get_x(lambdas) << "\n";
		//double tmpx = exp(d[i].get_x(lambdas));
		double tmpx = d[i].get_x_cond(lambdas, condlambda);
		snppri[i] = tmpx;
		//sumxs += tmpx;
		sumxs = sumlog(sumxs, tmpx);
	}
	for (int i = st; i <  sp ; i++) {
		//snppri[i] = snppri[i]/sumxs;
		//doing this in log space
		snppri[i] = snppri[i] - sumxs;
		if (!isfinite(snppri[i])){
			cerr << "ERROR: prior for SNP "<< i << " is " << snppri[i] << "\n";
			exit(1);
		}
		//if (snppri[i] < DBL_MIN) snppri[i] = DBL_MIN;
		//cout << i << " "<< snppri[i] << "\n";
	}
}

double SNPs::llk(){
	double toreturn = 0;
	for (int i = 0; i < segments.size(); i++) toreturn += llk(i);
	data_llk = toreturn;
	return toreturn;
}

double SNPs::llk_xv(set<int> skip, bool penalize){
	double toreturn = 0;
	for (int i = 0; i < segments.size(); i++) {
		if (skip.find(i) != skip.end()) continue;
		toreturn += llk(i);
	}
	if (penalize){
		for (vector<double>::iterator it = lambdas.begin(); it != lambdas.end(); it++) toreturn -= params->ridge_penalty * *it * *it;
		for (vector<double>::iterator it = seglambdas.begin(); it != seglambdas.end(); it++) toreturn -= params->ridge_penalty * *it * *it;

	}//data_llk = toreturn;
	return toreturn;
}

double SNPs::llk_ridge(){
	double toreturn = 0;
	for (int i = 0; i < segments.size(); i++) toreturn += llk(i);
	for (vector<double>::iterator it = lambdas.begin(); it != lambdas.end(); it++) toreturn -= params->ridge_penalty * *it * *it;
	for (vector<double>::iterator it = seglambdas.begin(); it != seglambdas.end(); it++) toreturn -= params->ridge_penalty * *it * *it;
	data_llk = toreturn;
	return toreturn;
}


double SNPs::llk(int which){

	double toreturn;

	pair<int, int> seg = segments[which];

	int st = seg.first;
	int sp = seg.second;
	double lsum = snppri[st]+ d[st].BF;
	//double lsum = log(snppri[st])+ d[st].BF;
	st++;
	for (int i = st; i < sp ; i++){
		double tmp2add = snppri[i]+ d[i].BF;
		//double tmp2add = log(snppri[i])+ d[i].BF;
		if (!isfinite(tmp2add)){
			cerr << "ERROR: likelihood of "<< tmp2add << " at SNP "<< d[i].id << " BF:"<< d[i].BF << " "<< snppri[i] << "\n";
			exit(1);
		}
		lsum  = sumlog(lsum, tmp2add);
		//cout << tmp2add << " "<< snppri[i]<< " "<< d[i].BF << " "<< lsum << "\n";
	}
	if (params->finemap) return lsum;
	toreturn = log(segpriors[which]) + lsum;
	//cout << toreturn << "\n";
	toreturn = sumlog(toreturn, log(1-segpriors[which]));

	return toreturn;
}

double SNPs::sumlog(double logx, double logy){
        if (logx > logy) return logx + log(1 + exp(logy-logx));
        else return logy + log(1 + exp(logx-logy));
}

void SNPs::set_post(){
	for (int i = 0; i < segments.size(); i++) set_post(i);
}
void SNPs::set_post(int which){
	pair<int, int> seg = segments[which];
	int st = seg.first;
	int sp = seg.second;
	double seglk = llk(which);
	for (int i = st; i < sp; i++){
		double num = log(segpi) + d[i].BF + snppri[i];
		//double num = log(segpi) + d[i].BF + log(snppri[i]);
		double lpost = num - seglk;
		snppost[i] = exp(lpost);
	}
}

void SNPs::print_segprobs(string outfile){
	ogzstream out(outfile.c_str());
	for (vector<pair<int, int> >::iterator it = segments.begin(); it != segments.end(); it++){
		int st = it->first;
		int sp = it->second;
		double total = 0;
		for (int i = st ; i < sp ; i++){
			total += snppost[i];
		}
		out << d[st].chr << " "<< d[st].pos << " "<< d[sp].pos << " " <<  total << "\n";
	}
}

void SNPs::optimize_segpi(){
	double start = log(segpi) - log(1-segpi);
	double min = -10.0;
	double max = 10.0;
	double tau = 0.001;
	golden_section_segpi(min, start, max, tau);
	cout << segpi << "\n";
}

void SNPs::optimize_condlambda(){
	double start = 0;
	double min = -20.0;
	double max = 20.0;
	double tau = 0.001;
	golden_section_condlambda(min, start, max, tau);
	cout << condlambda << "\n";
}


void SNPs::optimize_l0(){
	double start = lambdas[0];
	double min = -1000.0;
	double max = 1000.0;
	double tau = 0.001;
	golden_section_l0(min, start, max, tau);
	cout << lambdas[0] << "\n";
}


void SNPs::GSL_optim(){
	if (params->finemap){
		GSL_optim_fine();
		return;
	}
	int nparam = nannot+nsegannot+1;
	size_t iter = 0;
	double size;
    int status;
    const gsl_multimin_fminimizer_type *T =
    		gsl_multimin_fminimizer_nmsimplex2;
    gsl_multimin_fminimizer *s;
    gsl_vector *x;
    gsl_vector *ss;
    gsl_multimin_function lm;
    lm.n = nparam;
    lm.f = &GSL_llk;
    struct GSL_params p;
    p.d = this;
    lm.params = &p;
    //cout << llk()<< "\n"; cout.flush();
    //
    // initialize parameters
    //
    x = gsl_vector_alloc (nparam);
    gsl_vector_set(x, 0, log(segpi) - log(1-segpi));
    for (int i = 0; i < nparam-1; i++)   gsl_vector_set(x, i+1, 1);

    // set initial step sizes to 1
    ss = gsl_vector_alloc(nparam);
    gsl_vector_set_all(ss, 1.0);
    s = gsl_multimin_fminimizer_alloc (T, nparam);

    gsl_multimin_fminimizer_set (s, &lm, x, ss);
    do
     {
             iter++;
             status = gsl_multimin_fminimizer_iterate (s);

             if (status){
                     printf ("error: %s\n", gsl_strerror (status));
                     break;
             }
             size = gsl_multimin_fminimizer_size(s);
             status = gsl_multimin_test_size (size, 0.001);
             //cout << iter << " "<< iter %10 << "\n";
             if (iter % 20 < 1 || iter < 2){
            	 cout <<"iteration: "<< iter << " "<< segpi;
            	 for (int i = 0; i < nsegannot; i++) cout <<  " "<< seglambdas[i];
            	 for (int i = 0; i < nannot; i++) cout << " "<< lambdas[i];
            	 cout << " "<< s->fval << " "<< size <<  "\n";
             }

     }
     while (status == GSL_CONTINUE && iter <5000);
     if (iter > 4999) {
             cerr << "WARNING: failed to converge\n";
             //exit(1);
     }
     segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(s->x, 0)));
     for (int i = 0; i < nsegannot; i++) seglambdas[i] = gsl_vector_get(s->x, i+1);
     for (int i = 0; i < nannot; i++) lambdas[i] = gsl_vector_get(s->x, i+nsegannot+1);


     gsl_multimin_fminimizer_free (s);
     gsl_vector_free (x);
     gsl_vector_free(ss);
}


void SNPs::GSL_xv_optim(set<int> toskip, bool penalize){
	int nparam = nannot+nsegannot+1;
	size_t iter = 0;
	double size;
    int status;
    const gsl_multimin_fminimizer_type *T =
    		gsl_multimin_fminimizer_nmsimplex2;
    gsl_multimin_fminimizer *s;
    gsl_vector *x;
    gsl_vector *ss;
    gsl_multimin_function lm;
    lm.n = nparam;
    lm.f = &GSL_llk_xv;
    struct GSL_params p;
    p.d = this;
    p.toskip = toskip;
    p.penalize = penalize;
    lm.params = &p;
    //cout << llk()<< "\n"; cout.flush();
    //
    // initialize parameters
    //
    x = gsl_vector_alloc (nparam);
    gsl_vector_set(x, 0, log(segpi) - log(1-segpi));
    for (int i = 0; i < nparam-1; i++)   gsl_vector_set(x, i+1, 1);

    // set initial step sizes to 1
    ss = gsl_vector_alloc(nparam);
    gsl_vector_set_all(ss, 1.0);
    s = gsl_multimin_fminimizer_alloc (T, nparam);

    gsl_multimin_fminimizer_set (s, &lm, x, ss);
    do
     {
             iter++;
             status = gsl_multimin_fminimizer_iterate (s);

             if (status){
                     printf ("error: %s\n", gsl_strerror (status));
                     break;
             }
             size = gsl_multimin_fminimizer_size(s);
             status = gsl_multimin_test_size (size, 0.001);
             //cout << iter << " "<< iter %10 << "\n";
             if (iter % 20 < 1 || iter < 2){
            	 cout <<"iteration: "<< iter << " "<< segpi;
            	 for (int i = 0; i < nsegannot; i++) cout <<  " "<< seglambdas[i];
            	 for (int i = 0; i < nannot; i++) cout << " "<< lambdas[i];
            	 cout << " "<< s->fval << " "<< size <<  "\n";
             }

     }
     while (status == GSL_CONTINUE && iter <5000);
     if (iter > 4999) {
             cerr << "WARNING: failed to converge\n";
             //exit(1);
     }
     segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(s->x, 0)));
     for (int i = 0; i < nsegannot; i++) seglambdas[i] = gsl_vector_get(s->x, i+1);
     for (int i = 0; i < nannot; i++) lambdas[i] = gsl_vector_get(s->x, i+nsegannot+1);


     gsl_multimin_fminimizer_free (s);
     gsl_vector_free (x);
     gsl_vector_free(ss);
}




void SNPs::GSL_optim_fine(){
	int nparam = nannot;
	if (nparam < 1) return;
	size_t iter = 0;
	double size;
    int status;
    const gsl_multimin_fminimizer_type *T =
    		gsl_multimin_fminimizer_nmsimplex2;
    gsl_multimin_fminimizer *s;
    gsl_vector *x;
    gsl_vector *ss;
    gsl_multimin_function lm;
    lm.n = nparam;
    lm.f = &GSL_llk_fine;
    struct GSL_params p;
    p.d = this;
    lm.params = &p;
    //cout << llk()<< "\n"; cout.flush();
    //
    // initialize parameters
    //

    x = gsl_vector_alloc (nparam);
    for (int i = 0; i < nannot; i++)   gsl_vector_set(x, i, 1);

    // set initial step sizes to 1
    ss = gsl_vector_alloc(nparam);
    gsl_vector_set_all(ss, 1.0);

    s = gsl_multimin_fminimizer_alloc (T, nparam);
    //cout << "here o1\n"; cout.flush();
    gsl_multimin_fminimizer_set (s, &lm, x, ss);
    //cout << "here2\n"; cout.flush();
    do
     {
             iter++;
             status = gsl_multimin_fminimizer_iterate (s);

             if (status){
                     printf ("error: %s\n", gsl_strerror (status));
                     break;
             }
             size = gsl_multimin_fminimizer_size(s);
             status = gsl_multimin_test_size (size, 0.001);
             //cout << iter << " "<< iter %10 << "\n";
             if (iter % 20 < 1 || iter < 2){
            	 cout <<"iteration: "<< iter;
            	 for (int i = 0; i < nannot; i++) cout << " "<< lambdas[i];
            	 cout << " "<< s->fval << " "<< size <<  "\n";
             }

     }
     while (status == GSL_CONTINUE && iter <5000);
     if (iter > 4999) {
             cerr << "WARNING: failed to converge\n";
             //exit(1);
     }
     for (int i = 0; i < nannot; i++) lambdas[i] = gsl_vector_get(s->x, i);


     gsl_multimin_fminimizer_free (s);
     gsl_vector_free (x);
     gsl_vector_free(ss);
}

void SNPs::GSL_optim_ridge(){
	int nparam = nannot+nsegannot+1;
	size_t iter = 0;
	double size;
    int status;
    const gsl_multimin_fminimizer_type *T =
    		gsl_multimin_fminimizer_nmsimplex2;
    gsl_multimin_fminimizer *s;
    gsl_vector *x;
    gsl_vector *ss;
    gsl_multimin_function lm;
    lm.n = nparam;
    lm.f = &GSL_llk_ridge;
    struct GSL_params p;
    p.d = this;
    lm.params = &p;
    //cout << llk()<< "\n"; cout.flush();
    //
    // initialize parameters
    //
    x = gsl_vector_alloc (nparam);
    gsl_vector_set(x, 0, log(segpi) - log(1-segpi));
    for (int i = 0; i < nparam-1; i++)   gsl_vector_set(x, i+1, 1);

    // set initial step sizes to 1
    ss = gsl_vector_alloc(nparam);
    gsl_vector_set_all(ss, 1.0);
    s = gsl_multimin_fminimizer_alloc (T, nparam);

    gsl_multimin_fminimizer_set (s, &lm, x, ss);
    do
     {
             iter++;
             status = gsl_multimin_fminimizer_iterate (s);

             if (status){
                     printf ("error: %s\n", gsl_strerror (status));
                     break;
             }
             size = gsl_multimin_fminimizer_size(s);
             status = gsl_multimin_test_size (size, 0.001);
             //cout << iter << " "<< iter %10 << "\n";
             if (iter % 20 < 1 || iter < 2){
            	 cout <<"iteration: "<< iter << " "<< segpi;
            	 for (int i = 0; i < nsegannot; i++) cout <<  " "<< seglambdas[i];
            	 for (int i = 0; i < nannot; i++) cout << " "<< lambdas[i];
            	 cout << " "<< s->fval << " "<< size <<  "\n";
             }

     }
     while (status == GSL_CONTINUE && iter <5000);
     if (iter > 4999) {
             cerr << "WARNING: failed to converge\n";
             //exit(1);
     }
     segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(s->x, 0)));
     for (int i = 0; i < nsegannot; i++) seglambdas[i] = gsl_vector_get(s->x, i+1);
     for (int i = 0; i < nannot; i++) lambdas[i] = gsl_vector_get(s->x, i+nsegannot+1);


     gsl_multimin_fminimizer_free (s);
     gsl_vector_free (x);
     gsl_vector_free(ss);
}

int SNPs::golden_section_segpi(double min, double guess, double max, double tau){
        double x;

        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                segpi =  1.0  / ( 1.0 + exp(-new_segpi));
                data_llk = llk();
                return 0;
        }

        segpi = 1.0  / ( 1.0 + exp(-x));
        double f_x = -llk();

        segpi =  1.0  / ( 1.0 + exp(-guess));
        double f_guess = -llk();
        cout << x << " " <<  guess << " "<< segpi << " "<< f_x << " "<< f_guess <<  "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_segpi(guess, x, max, tau);
                else return golden_section_segpi(min, x, guess, tau);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_segpi(min, guess, x, tau);
                else return golden_section_segpi(x, guess, max, tau);
        }
}


int SNPs::golden_section_condlambda(double min, double guess, double max, double tau){
        double x;
        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                condlambda =  new_segpi;
                set_priors_cond();
                data_llk = llk();
                return 0;
        }

        condlambda= x;
        set_priors_cond();
        double f_x = -llk();

        condlambda = guess;
        set_priors_cond();
        double f_guess = -llk();

        cout << x << " " <<  guess << " "<< f_x << " "<< f_guess  << " "<< max << " "<< min << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_condlambda(guess, x, max, tau);
                else return golden_section_condlambda(min, x, guess, tau);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_condlambda(min, guess, x, tau);
                else return golden_section_condlambda(x, guess, max, tau);
        }
}

int SNPs::golden_section_condlambda_ci(double min, double guess, double max, double tau, double target){
        double x;

        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                condlambda =  new_segpi;
                set_priors_cond();
                data_llk = llk();
                double tmpdiff = data_llk- target;
                double d2 = tmpdiff*tmpdiff;
                if (d2 > tau) return 1;
                else return 0;
        }

        condlambda = x;
        set_priors_cond();
        double f_x = llk()-target;
        f_x = f_x*f_x;
       // double x_llk = llk();

        condlambda = guess;
        set_priors_cond();
        double f_guess = llk()-target;
        f_guess = f_guess*f_guess;
       // double guess_llk = llk();
        cout << x << " " <<  guess << " "<< f_x << " "<< f_guess  << " "<< max << " "<< min << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_condlambda_ci(guess, x, max, tau, target);
                else return golden_section_condlambda_ci(min, x, guess, tau, target);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_condlambda_ci(min, guess, x, tau, target);
                else return golden_section_condlambda_ci(x, guess, max, tau, target);
        }
}


int SNPs::golden_section_lambda_ci(double min, double guess, double max, double tau, int which, double target){
        double x;

        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                lambdas[which] =  new_segpi;
                set_priors();
                data_llk = llk();
                double tmpdiff = data_llk- target;
                double d2 = tmpdiff*tmpdiff;
                if (d2 > tau) return 1;
                else return 0;
        }

        lambdas[which] = x;
        set_priors();
        double f_x = llk()-target;
        f_x = f_x*f_x;
       // double x_llk = llk();

        lambdas[which] = guess;
        set_priors();
        double f_guess = llk()-target;
        f_guess = f_guess*f_guess;
       // double guess_llk = llk();
        cout << x << " " <<  guess << " "<< f_x << " "<< f_guess  << " "<< max << " "<< min << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_lambda_ci(guess, x, max, tau, which, target);
                else return golden_section_lambda_ci(min, x, guess, tau, which, target);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_lambda_ci(min, guess, x, tau, which, target);
                else return golden_section_lambda_ci(x, guess, max, tau, which, target);
        }
}


int SNPs::golden_section_seglambda_ci(double min, double guess, double max, double tau, int which, double target){
        double x;

        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                seglambdas[which] =  new_segpi;
                set_priors();
                data_llk = llk();
                double tmpdiff = data_llk- target;
                double d2 = tmpdiff*tmpdiff;
                if (d2 > tau) return 1;
                else return 0;
        }

        seglambdas[which] = x;
        set_priors();
        double f_x = llk()-target;
        f_x = f_x*f_x;
        //double x_llk = llk();

        seglambdas[which] = guess;
        set_priors();
        double f_guess = llk()-target;
        f_guess = f_guess*f_guess;
        //double guess_llk = llk();
        cout << x << " " <<  guess << " "<< f_x << " "<< f_guess << " "<< max << " "<< min << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_seglambda_ci(guess, x, max, tau, which, target);
                else return golden_section_seglambda_ci(min, x, guess, tau, which, target);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_seglambda_ci(min, guess, x, tau, which, target);
                else return golden_section_seglambda_ci(x, guess, max, tau, which, target);
        }
}

int SNPs::golden_section_segpi_ci(double min, double guess, double max, double tau, double target, int * nit){
        double x;
        *nit = *nit +1;
        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                segpi =  1.0  / ( 1.0 + exp(-new_segpi));
                set_priors();
                data_llk = llk();
                double tmpdiff = data_llk- target;
                double d2 = tmpdiff*tmpdiff;
                if (d2 > tau) return 1;
                else return 0;
        }
        else if (*nit > 50) return 1;

        segpi = 1.0  / ( 1.0 + exp(-x));
        set_priors();
        double f_x = llk()-target;
        f_x = f_x*f_x;

        segpi =  1.0  / ( 1.0 + exp(-guess));
        set_priors();
        double f_guess = llk()- target;
        f_guess = f_guess*f_guess;

        cout << x << " " <<  guess << " "<< f_x << " "<< f_guess <<  " "<< max << " "<< min << " "<< target << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_segpi_ci(guess, x, max, tau, target, nit);
                else return golden_section_segpi_ci(min, x, guess, tau, target, nit);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_segpi_ci(min, guess, x, tau, target, nit);
                else return golden_section_segpi_ci(x, guess, max, tau, target, nit);
        }
}


int SNPs::golden_section_l0(double min, double guess, double max, double tau){
        double x;

        if ( (max - guess) > (guess - min)) x = guess + resphi *( max - guess);
        else x = guess - resphi *(guess-min);
        if (fabs(max-min) < tau * (fabs(guess)+fabs(max))) {
                double new_segpi = (min+max)/2;
                lambdas[0] =  new_segpi;
                data_llk = llk();
                return 0;
        }

        lambdas[0] = x;
        set_priors();
        double f_x = -llk();

        lambdas[0] = guess;
        set_priors();
        double f_guess = -llk();
        cout << x << " " <<  guess << " "<< segpi << " "<< f_x << " "<< f_guess <<  " "<< max << " "<< min << "\n";
        if (f_x < f_guess){
                if ( (max-guess) > (guess-min) )        return golden_section_l0(guess, x, max, tau);
                else return golden_section_l0(min, x, guess, tau);
        }
        else{
                if ( (max - guess) > (guess - min)  ) return golden_section_l0(min, guess, x, tau);
                else return golden_section_l0(x, guess, max, tau);
        }
}


double GSL_llk(const gsl_vector *x, void *params ){
	//first set times
	int na = ((struct GSL_params *) params)->d->nannot;
	int ns = ((struct GSL_params *) params)->d->nsegannot;
	((struct GSL_params *) params)->d->segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(x, 0)));

	for (int i = 0; i < ns; i++){
		((struct GSL_params *) params)->d->seglambdas[i] = gsl_vector_get(x, i+1);
	}

	for (int i = 0; i < na; i++){
		((struct GSL_params *) params)->d->lambdas[i] = gsl_vector_get(x, i+ns+1);
	}
	((struct GSL_params *) params)->d->set_priors();
	return -((struct GSL_params *) params)->d->llk();
}

double GSL_llk_xv(const gsl_vector *x, void *params ){
	//first set times
	int na = ((struct GSL_params *) params)->d->nannot;
	int ns = ((struct GSL_params *) params)->d->nsegannot;
	set<int> toskip = ((struct GSL_params *) params)->toskip;
	bool penalize  = ((struct GSL_params *) params)->penalize;
	((struct GSL_params *) params)->d->segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(x, 0)));

	for (int i = 0; i < ns; i++){
		((struct GSL_params *) params)->d->seglambdas[i] = gsl_vector_get(x, i+1);
	}

	for (int i = 0; i < na; i++){
		((struct GSL_params *) params)->d->lambdas[i] = gsl_vector_get(x, i+ns+1);
	}
	((struct GSL_params *) params)->d->set_priors();
	return -((struct GSL_params *) params)->d->llk_xv(toskip, penalize);
}


double GSL_llk_ridge(const gsl_vector *x, void *params ){
	//first set times
	int na = ((struct GSL_params *) params)->d->nannot;
	int ns = ((struct GSL_params *) params)->d->nsegannot;
	((struct GSL_params *) params)->d->segpi = 1.0 /  (1.0 + exp (- gsl_vector_get(x, 0)));

	for (int i = 0; i < ns; i++){
		((struct GSL_params *) params)->d->seglambdas[i] = gsl_vector_get(x, i+1);
	}

	for (int i = 0; i < na; i++){
		((struct GSL_params *) params)->d->lambdas[i] = gsl_vector_get(x, i+ns+1);
	}
	((struct GSL_params *) params)->d->set_priors();
	return -((struct GSL_params *) params)->d->llk_ridge();
}

double GSL_llk_fine(const gsl_vector *x, void *params ){
	//first set times
	int na = ((struct GSL_params *) params)->d->nannot;

	for (int i = 0; i < na; i++){
		((struct GSL_params *) params)->d->lambdas[i] = gsl_vector_get(x, i);
	}
	((struct GSL_params *) params)->d->set_priors();
	return -((struct GSL_params *) params)->d->llk();
}


