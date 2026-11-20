#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
struct Vec { float x,y; };
inline Vec operator+(Vec a,Vec b){return {a.x+b.x,a.y+b.y};}
inline Vec operator-(Vec a,Vec b){return {a.x-b.x,a.y-b.y};}
inline Vec operator*(Vec a,float b){return {a.x*b,a.y*b};}
inline float length2(Vec a){return a.x*a.x+a.y*a.y;}

// A cell-centered finite-volume film. Depth and both components of momentum
// cross faces together. The magnetic potential is an external pressure head.
struct Bath {
    static constexpr int NX=192,NY=72,S=NX*NY,W=961,H=361;
    static constexpr float aspect=8.f/3,dx=1.f/NY,g=6.f;
    struct Cell{float h=0,x=0,y=0;};
    struct Pole{Vec pos{};float stress=0;Vec anchor{};};
    float holdMotion=.85f,holdHz=.60f,phase=0;
    float radius=.15f,viscosity=.88f,tension=1,priority=5,follow=9,returnRate=5.5f;
    float stretch=1.65f,spikes=1,spacing=.040f,edge=.9f;
    const Vec home{aspect*.5f,.51f};
    std::array<Cell,S> film{},delta{};
    std::array<float,S> bed{},head{},rough{},priorDepth{},homeHead{};
    std::array<uint16_t,W*H*2> surface{};
    std::array<Pole,128> poles{};
    std::array<Pole,8> fading{};
    int active=0,x0=0,x1=W-1,y0=0,y1=H-1;
    float quiet=0,previousRadius=0,stressError=0,maxHeadChange=0;
    bool asleep=false;
    int l=0,r=NX-1,t=0,b=NY-1;
    double initialMass=0;
    static float bowl(float d2,float scale,float range){
        return scale*d2/(1+sqrtf(d2)/range);
    }
    Bath(){
        for(int j=0;j<NY;j++)for(int i=0;i<NX;i++){
            float x=(i+.5f)*dx,y=(j+.5f)*dx;
            rough[j*NX+i]=.0028f*(sinf(21*x+11*y)+.4f*cosf(39*y-23*x));
        }
        initialize();
    }
    void initialize(){
        previousRadius=radius;
        float level=bowl(radius*radius,3.8f,.48f);
        initialMass=0;
        for(int j=0;j<NY;j++)for(int i=0;i<NX;i++){
            int n=j*NX+i;Vec p{(i+.5f)*dx,(j+.5f)*dx};
            homeHead[n]=bowl(length2(p-home),3.8f,.48f);
            bed[n]=homeHead[n]+edge*rough[n];
            film[n]={std::max(0.f,level-bed[n]),0,0};initialMass+=film[n].h;
        }
        sleepReset();
    }
    void sleepReset(){quiet=0;asleep=false;l=0;r=NX-1;t=0;b=NY-1;}
    void tap(Vec where,float strength){
        quiet=0;asleep=false;
        for(int j=t;j<=b;j++)for(int i=l;i<=r;i++){
            auto& c=film[j*NX+i];Vec d=where-Vec{(i+.5f)*dx,(j+.5f)*dx};
            float kick=.075f*strength*c.h/(.18f+sqrtf(length2(d)));
            c.x+=d.x*kick;c.y+=d.y*kick;
        }
    }
    void field(const Vec* keys,int count,float strength,float dt){
        auto old=poles;int oldCount=active;active=count;stressError=0;
        if(count)phase=fmodf(phase+6.2831853f*holdHz*dt,6.2831853f*24);
        for(int j=0;j<oldCount;j++){
            bool kept=false;for(int k=0;k<count;k++)kept|=length2(keys[k]-old[j].anchor)<1e-7f;
            if(!kept&&old[j].stress>.003f){
                auto slot=std::min_element(fading.begin(),fading.end(),[](Pole a,Pole b){return a.stress<b.stress;});
                *slot=old[j];
            }
        }
        for(auto& p:fading){p.stress*=expf(-7*dt);if(p.stress<.001f)p.stress=0;stressError=std::max(stressError,p.stress);}
        float weight=1;
        for(int k=count-1;k>=0;k--){
            float s=0;
            for(int j=0;j<oldCount;j++)if(length2(keys[k]-old[j].anchor)<1e-7f)s=old[j].stress;
            for(auto& p:fading)if(length2(p.anchor-keys[k])<1e-7f&&p.stress>0){s=std::max(s,p.stress);p.stress=0;}
            float angle=phase+keys[k].x*2.6f+keys[k].y;
            float modulation=.27f*sinf(angle)+.10f*sinf(2*angle+.8f);
            float wanted=strength*weight*(1+holdMotion*modulation);
            float gain=holdMotion*radius;
            Vec focus=keys[k]+Vec{.23f*cosf(angle)+.045f*sinf(2*angle),.13f*sinf(angle)}*gain;
            poles[k]={focus,wanted+(s-wanted)*expf(-7.5f*dt),keys[k]};
            stressError=std::max(stressError,fabsf(wanted-poles[k].stress));
            weight/=sqrtf(priority);
        }
        float relax=1-expf(-dt*(count?follow:returnRate)*1.55f);
        maxHeadChange=0;
        for(int j=0;j<NY;j++)for(int i=0;i<NX;i++){
            int n=j*NX+i;Vec xy{(i+.5f)*dx,(j+.5f)*dx};
            float potential=homeHead[n];
            if(count){
                Vec d=xy-keys[count-1];float d2=length2(d);
                // A long-range tilt transports the film; short-range wells attract the front first.
                float gain=std::clamp(follow/9,.5f,1.7f);
                float localD=length2(xy-poles[count-1].pos);
                potential=gain*bowl(d2,5.2f,.56f)-(.10f+.043f*stretch)*poles[count-1].stress/(1+localD/(radius*radius*.65f));
                for(int k=count-2;k>=0&&poles[k].stress>.008f;k--){
                    float q=length2(xy-keys[k])/(radius*radius*.21f);
                    potential-=.82f*poles[k].stress/((1+q)*(1+q));
                }
            }
            float change=(potential+rough[n]*edge-bed[n])*relax;
            if(film[n].h>.002f)maxHeadChange=std::max(maxHeadChange,fabsf(change));
            bed[n]+=change;
        }
    }
    // Hydrostatic reconstruction followed by a local Lax-Friedrichs flux.
    // Equal and opposite mass fluxes ensure that a key can never create fluid.
    void face(int a,int bIndex,bool vertical,float coeff){
        Cell A=film[a],B=film[bIndex];
        if(A.h+B.h<1e-10f)return;
        float slope=head[bIndex]-head[a];
        float scale=.65f*std::max(A.h,B.h);
        float flow=(fabsf(A.x)+fabsf(A.y)+fabsf(B.x)+fabsf(B.y))/(.001f+A.h+B.h);
        float moving=std::clamp((flow-.04f)/.28f,0.f,1.f);
        float dz=slope+moving*(std::clamp(slope,-scale,scale)-slope);
        // A dry uphill face is already a hydrostatic shore. On steep wet faces
        // apply the unresolved part of the magnetic gradient as a body force.
        if((A.h==0 && slope<0)||(B.h==0 && slope>0))dz=slope;
        float ha=std::max(0.f,A.h-std::max(0.f,dz)),hb=std::max(0.f,B.h+std::min(0.f,dz));
        float ia=A.h>.00001f?1/A.h:0,ib=B.h>.00001f?1/B.h:0;
        float ax=A.x*ia,ay=A.y*ia,bx=B.x*ib,by=B.y*ib;
        float ua=vertical?ay:ax,ub=vertical?by:bx;
        float wave=std::max(fabsf(ua)+sqrtf(g*ha),fabsf(ub)+sqrtf(g*hb));
        float mass=.5f*coeff*(ha*ua+hb*ub-wave*(hb-ha));
        float fx=.5f*coeff*(ha*ax*ua+hb*bx*ub-wave*(hb*bx-ha*ax));
        float fy=.5f*coeff*(ha*ay*ua+hb*by*ub-wave*(hb*by-ha*ay));
        // Viscous stress is symmetric at the face, including tangential momentum.
        float visc=coeff*.0016f*viscosity*std::min(ha,hb)/dx;
        fx-=visc*(bx-ax);fy-=visc*(by-ay);
        auto& da=delta[a];auto& db=delta[bIndex];
        da.h-=mass;db.h+=mass;da.x-=fx;db.x+=fx;da.y-=fy;db.y+=fy;
        float p=.25f*g*coeff*(ha*ha+hb*hb);
        float pa=p+.5f*g*coeff*(A.h*A.h-ha*ha),pb=p+.5f*g*coeff*(B.h*B.h-hb*hb);
        float residual=.5f*g*coeff*(slope-dz);
        if(vertical){da.y-=pa+residual*A.h;db.y+=pb-residual*B.h;}
        else{da.x-=pa+residual*A.h;db.x+=pb-residual*B.h;}
    }
    bool step(const Vec* keys,int count,float strength,float dt){
        if(radius!=previousRadius){
            // Changing the volume setting scales the existing film in place.
            float ratio=radius/previousRadius;
            for(auto& c:film){c.h*=ratio*ratio*ratio;c.x*=ratio*ratio*ratio;c.y*=ratio*ratio*ratio;}
            previousRadius=radius;sleepReset();
        }
        field(keys,count,strength,dt);
        for(int n=0;n<S;n++)priorDepth[n]=film[n].h;
        float remaining=dt;
        while(remaining>1e-6f){
            float maxSignal=.6f;
            for(int j=t;j<=b;j++)for(int i=l;i<=r;i++){
                int n=j*NX+i;const Cell& c=film[n];
                delta[n]={};
                float lap=film[j*NX+std::max(i-1,0)].h+film[j*NX+std::min(i+1,NX-1)].h
                         +film[std::max(j-1,0)*NX+i].h+film[std::min(j+1,NY-1)*NX+i].h-4*c.h;
                head[n]=bed[n]-tension*(.07f*lap+.015f*c.h/(c.h+.008f));
                if(c.h>.00001f)maxSignal=std::max(maxSignal,(fabsf(c.x)+fabsf(c.y))/c.h+2*sqrtf(g*c.h));
            }
            float h=std::min({remaining,1.f/240,.42f*dx/(maxSignal+.5f)});remaining-=h;
            float coeff=h/dx;
            for(int j=t;j<=b;j++)for(int i=l;i<=r;i++){
                int n=j*NX+i;
                if(i<r)face(n,n+1,false,coeff);
                if(j<b)face(n,n+NX,true,coeff);
            }
            int nl=NX-1,nr=0,nt=NY-1,nb=0;
            for(int j=t;j<=b;j++)for(int i=l;i<=r;i++){
                int n=j*NX+i;auto& c=film[n];auto d=delta[n];
                // Solid walls reflect normal momentum and exchange no mass.
                float wall=.5f*g*c.h*c.h*coeff;
                if(i==0)d.x+=wall;if(i==NX-1)d.x-=wall;
                if(j==0)d.y+=wall;if(j==NY-1)d.y-=wall;
                float nh=std::max(0.f,c.h+d.h);
                float drag=(count?2.75f:2.35f)+viscosity*1.65f+.005f/(nh+.0025f);
                if(!count)drag*=5.5f/returnRate;
                float decay=1/(1+h*drag);
                c={nh,(c.x+d.x)*decay,(c.y+d.y)*decay};
                if(nh<.00001f){c.x=0;c.y=0;}
                else{
                    float speed2=(c.x*c.x+c.y*c.y)/(nh*nh);
                    if(speed2>16){float f=4/sqrtf(speed2);c.x*=f;c.y*=f;}

                }
                if(nh>1e-9f){nl=std::min(nl,i);nr=std::max(nr,i);nt=std::min(nt,j);nb=std::max(nb,j);}
            }
            l=std::max(0,nl-2);r=std::min(NX-1,nr+2);t=std::max(0,nt-2);b=std::min(NY-1,nb+2);
        }
        float shifted=0,maxHeightDelta=0;
        for(int n=0;n<S;n++){
            float difference=fabsf(film[n].h-priorDepth[n]);
            shifted+=difference;maxHeightDelta=std::max(maxHeightDelta,difference);
        }
        if(shifted<initialMass*.012f*dt && maxHeightDelta<.004f*dt && maxHeadChange<.0003f && stressError<.003f)
            quiet+=dt;
        else quiet=0;
        asleep=quiet>.20f && !(count && holdMotion>.001f);
        return !asleep;
    }
    static float spline(float a,float b,float c,float d,float f){
        return b+.5f*f*((c-a)+f*((2*a-5*b+4*c-d)+f*(3*(b-c)+d-a)));
    }
    float sample(float x,float y)const{
        int i=(int)floorf(x),j=(int)floorf(y);float row[4];
        for(int k=-1;k<=2;k++){
            auto h=[&](int u){return film[std::clamp(j+k,0,NY-1)*NX+std::clamp(i+u,0,NX-1)].h;};
            row[k+1]=spline(h(-1),h(0),h(1),h(2),x-i);
        }
        return std::max(0.f,spline(row[0],row[1],row[2],row[3],y-j));
    }
    float peaks(Vec q,const Pole& pole)const{
        float d2=length2(q);
        if(d2>radius*radius*3.4f)return 0;
        float local=pole.stress*expf(-d2/(radius*radius*.81f));
        if(local<.065f)return 0;
        // A field threshold separates smooth domes from sharp Rosensweig peaks.
        float sharpen=std::clamp((local-.18f)/.57f,0.f,1.f);
        float size=spacing*(.92f+.08f*cosf(q.x*22));
        Vec plane=q*(1+.18f*std::min(1.f,sqrtf(d2)/radius));
        float a=plane.x/size+.14f*sinf(plane.y*39);
        float b=plane.y/(size*.866f)+.12f*sinf(plane.x*36);
        float nearest=1;
        for(int z=-1;z<=1;z++){
            int row=(int)floorf(b)+z;float dx=a-.5f*(row&1);dx-=roundf(dx);
            float dy=(b-row)*.866f;nearest=std::min(nearest,dx*dx+dy*dy);
        }
        float t=std::min(1.f,sqrtf(nearest)/.62f);
        float bud=(1-t*t)*(1-t*t), cone=(1-t)*(1-t);
        return local*spikes*(.020f+.105f*sharpen)*(bud+(cone-bud)*sharpen-.13f);
    }
    static constexpr int tilesAcross=(W-1)/12,tilesDown=(H-1)/12;
    std::array<unsigned char,tilesAcross*tilesDown> wetTiles{};
    int previousX0=0,previousY0=0,previousX1=W-1,previousY1=H-1;
    static constexpr float cubicReach=.0003f;
    void markWetTiles(){
        wetTiles.fill(0);
        for(int y=t;y<=b;y++)for(int x=l;x<=r;x++){
            if(film[y*NX+x].h<cubicReach)continue;
            // A Catmull-Rom stencil spans four samples. Include all touched mesh
            // cells plus the adjacent tile sharing an edge vertex.
            int left=std::max(0,(5*x-9)/12),right=std::min(tilesAcross-1,(5*x+13)/12);
            int top=std::max(0,(5*y-9)/12),bottom=std::min(tilesDown-1,(5*y+13)/12);
            for(int yy=top;yy<=bottom;yy++)
                std::fill(&wetTiles[yy*tilesAcross+left],wetTiles.data()+yy*tilesAcross+right+1,1);
        }
    }
    // Separable cubic interpolation. Cache each horizontal row once and use
    // exactly the original spline polynomial for both passes.
    std::array<std::array<float,W>,4> horizontal{};
    void raster(){
        previousX0=x0;previousX1=x1;previousY0=y0;previousY1=y1;
        for(int j=y0;j<=y1;j++)
            std::fill(surface.data()+2*(j*W+x0),surface.data()+2*(j*W+x1+1),0);
        markWetTiles();
        int minX=NX-1,maxX=0,minY=NY-1,maxY=0;
        for(int j=t;j<=b;j++)for(int i=l;i<=r;i++)if(film[j*NX+i].h>.00065f){
            minX=std::min(minX,i);maxX=std::max(maxX,i);minY=std::min(minY,j);maxY=std::max(maxY,j);
        }
        x0=std::max(0,(minX-1)*5);x1=std::min(W-1,(maxX+2)*5);
        y0=std::max(0,(minY-1)*5);y1=std::min(H-1,(maxY+2)*5);
        int lastRow=-100;
        for(int j=y0;j<=y1;j++){
            float y=j*.2f-.5f;int row=(int)floorf(y);
            for(int k=std::max(row-1,lastRow+1);k<=row+2;k++){
                float* cache=horizontal[(k+4)%4].data();
                const Cell* cells=film.data()+std::clamp(k,0,NY-1)*NX;
                for(int i=x0;i<=x1;i++){
                    float u=i*.2f-.5f;int base=(int)floorf(u);
                    cache[i]=spline(cells[std::clamp(base-1,0,NX-1)].h,cells[std::clamp(base,0,NX-1)].h,
                                    cells[std::clamp(base+1,0,NX-1)].h,cells[std::clamp(base+2,0,NX-1)].h,u-base);
                }
            }
            lastRow=row+2;
            for(int i=x0;i<=x1;i++){
                float depth=spline(horizontal[(row+3)%4][i],horizontal[(row+4)%4][i],
                                   horizontal[(row+5)%4][i],horizontal[(row+6)%4][i],y-row);
                if(depth<.0013f)continue;
                Vec xy{i/360.f,j/360.f};
                float d=depth-.0013f;
                float wet=d/(d+.0012f);
                float base=.58f*depth+.020f*d/(d+.006f);
                float peak=0;
                for(int k=active-1;k>=0;k--)if(poles[k].stress>.02f)peak+=peaks(xy-poles[k].pos,poles[k]);
                for(const auto& p:fading)if(p.stress>.005f)peak+=peaks(xy-p.pos,p);
                float height=std::max(0.f,base+peak*depth/(depth+.021f));
                int n=2*(j*W+i);
                surface[n]=(uint16_t)(65535*std::min(.999f,height/.32f));
                surface[n+1]=(uint16_t)(65535*wet);
            }
        }
    }
};

