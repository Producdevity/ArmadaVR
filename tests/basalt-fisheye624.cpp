#include <basalt/camera/fisheye624_camera.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

static unsigned cases, defects;
static void check(bool value) { ++cases; defects += !value; }
using Parameters = std::array<long double, 16>;
using Point = std::array<long double, 4>;
static std::array<long double, 2> reference(const Parameters& p, const Point& point) {
  long double x = point[0] / point[2], y = point[1] / point[2];
  long double r = hypotl(x, y), theta = atanl(r), power = theta, radius = theta;
  for (int i = 0; i < 6; ++i) { power *= theta * theta; radius += p[4 + i] * power; }
  long double u = r ? x * radius / r : 0, v = r ? y * radius / r : 0, r2 = u*u + v*v;
  return {p[0]*(u+p[10]*(3*u*u+v*v)+2*p[11]*u*v+p[12]*r2+p[13]*r2*r2)+p[2],
          p[1]*(v+p[11]*(u*u+3*v*v)+2*p[10]*u*v+p[14]*r2+p[15]*r2*r2)+p[3]};
}
template<class S> static void run() {
  using C = basalt::Fisheye624Camera<S>;
  const long double tolerance = std::is_same_v<S,float> ? 0.003L : 0.000003L;
  for (unsigned model = 0; model < 3; ++model) {
    typename C::VecN parameters; parameters << 240, 245, 320, 240, .03, -.002, .0001, -.00001, .000001, -.0000001,
        .001, -.002, 0, 0, 0, 0;
    if (model == 0) parameters.template tail<12>().setZero();
    if (model == 2) parameters.template tail<4>() << .002, .0001, -.001, -.0002;
    Parameters p; for (unsigned i=0;i<16;++i) p[i]=parameters[i];
    C camera(parameters);
    for (double radius : {0., 1e-12, 1e-8, 1e-6, .001, .01, .1, .5, 1., 3.})
      for (unsigned direction=0; direction<4; ++direction) {
        S x=S(radius)*(direction&1?-1:1), y=S(radius*.37)*(direction&2?-1:1);
        typename C::Vec4 point(x,y,S(1.7),0), ray;
        typename C::Vec2 pixel; typename C::Mat24 jp; typename C::Mat2N jc;
        bool projected=camera.project(point,pixel,&jp,&jc);
        check(projected && pixel.allFinite() && jp.allFinite() && jc.allFinite());
        Point v;for(unsigned i=0;i<4;++i)v[i]=point[i];auto expected=reference(p,v);
        check(pixel.allFinite() && fabsl(pixel[0]-expected[0])<tolerance && fabsl(pixel[1]-expected[1])<tolerance);
        bool inverse=camera.unproject(pixel,ray);
        check(inverse && ray.allFinite() && (ray.template head<3>()-point.template head<3>().normalized()).norm() <
              (std::is_same_v<S,float>?1e-5:1e-11));
        for(unsigned column=0;column<4;++column) {
          Point lo=v,hi=v;lo[column]-=1e-5L;hi[column]+=1e-5L;
          auto a=reference(p,lo),b=reference(p,hi);
          for(unsigned row=0;row<2;++row) {
            long double derivative=(b[row]-a[row])/2e-5L;
            check(std::isfinite(jp(row,column)) && fabsl(jp(row,column)-derivative)<tolerance*fmaxl(1,fabsl(derivative)));
          }
        }
        for(unsigned column=0;column<16;++column) {
          Parameters lo=p,hi=p;lo[column]-=1e-5L;hi[column]+=1e-5L;
          auto a=reference(lo,v),b=reference(hi,v);
          for(unsigned row=0;row<2;++row) {
            long double derivative=(b[row]-a[row])/2e-5L;
            check(std::isfinite(jc(row,column)) && fabsl(jc(row,column)-derivative)<tolerance*fmaxl(1,fabsl(derivative)));
          }
        }
      }
  }
  typename C::VecN p; p.setZero();p[0]=p[1]=200;p[2]=320;p[3]=240;C camera(p);
  typename C::Vec4 output, point(0,0,0,0);typename C::Vec2 pixel;
  check(!camera.project(point,pixel));point[2]=-1;check(!camera.project(point,pixel));
  point[2]=1;point[0]=std::numeric_limits<S>::quiet_NaN();check(!camera.project(point,pixel));
  pixel<<std::numeric_limits<S>::quiet_NaN(),240;check(!camera.unproject(pixel,output));
  pixel<<1000000,240;check(!camera.unproject(pixel,output));
  p[0]=0;camera=C(p);point<<0,0,1,0;pixel<<320,240;
  check(!camera.project(point,pixel));check(!camera.unproject(pixel,output));
}
int main() {
  run<float>();run<double>();
  std::printf("BASALT_FISHEYE624 cases=%u defects=%u\n",cases,defects);
  return defects!=0;
}
