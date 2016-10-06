/*
 * Hydro.cc
 *
 *  Created on: Dec 22, 2011
 *      Author: cferenba
 *
 * Copyright (c) 2012, Los Alamos National Security, LLC.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style open-source
 * license; see top-level LICENSE file for full license text.
 */

#include "Hydro.hh"

#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <limits>

#include "CorrectorTask.hh"
#include "Memory.hh"
#include "PolyGas.hh"
#include "TTS.hh"
#include "QCS.hh"
#include "HydroBC.hh"
#include "LocalMesh.hh"

using namespace std;

// TODO: declare const initialized in all constructors as const
Hydro::Hydro(const InputParameters& params, LocalMesh* m,
		DynamicCollective add_reduction,
        Context ctx, HighLevelRuntime* rt) :
		mesh(m),
		cfl(params.directs.cfl),
		cflv(params.directs.cflv),
		rho_init(params.directs.rho_init),
		energy_init(params.directs.energy_init),
		rho_init_sub(params.directs.rho_init_sub),
		energy_init_sub(params.directs.energy_init_sub),
		vel_init_radial(params.directs.vel_init_radial),
		bcx(params.bcx),
		bcy(params.bcy),
		add_reduction(add_reduction),
		ctx(ctx),
		runtime(rt),
        zones(ctx, rt),
        sides_and_corners(ctx, rt),
        edges(ctx, rt),
        points(ctx, rt),
        params(params),
		my_color(params.directs.task_id)
{
    pgas = new PolyGas(params, this);
    tts = new TTS(params, this);
    qcs = new QCS(params, this);

    init();
}


Hydro::~Hydro() {

    delete tts;
    delete qcs;
}


void Hydro::init() {

    const int numpch = mesh->num_pt_chunks();
    const int numzch = mesh->num_zone_chunks();
    const int nump = mesh->num_pts;
    const int numz = mesh->num_zones;
    const int nums = mesh->num_sides;

    const double2* zx = mesh->zone_x;
    const double* zvol = mesh->zone_vol;

    // allocate arrays
    allocateFields();

    points.allocate(nump);
    pt_vel = points.getRawPtr<double2>(FID_PU);
    pt_vel0 = points.getRawPtr<double2>(FID_PU0);
    pt_accel = points.getRawPtr<double2>(FID_PAP);

    sides_and_corners.allocate(nums);
    crnr_weighted_mass = sides_and_corners.getRawPtr<double>(FID_CMASWT);
    side_force_pres = sides_and_corners.getRawPtr<double2>(FID_SFP);
    side_force_visc = sides_and_corners.getRawPtr<double2>(FID_SFQ);
    side_force_tts = sides_and_corners.getRawPtr<double2>(FID_SFT);
    crnr_force_tot = sides_and_corners.getRawPtr<double2>(FID_CFTOT);

    zones.allocate(numz);
    zone_rho = zones.getRawPtr<double>(FID_ZR);
    zone_rho_pred = zones.getRawPtr<double>(FID_ZRP);
    zone_energy_density = zones.getRawPtr<double>(FID_ZE);
    zone_pressure_ = zones.getRawPtr<double>(FID_ZP);
    zone_mass = zones.getRawPtr<double>(FID_ZM);
    zone_energy_tot = zones.getRawPtr<double>(FID_ZETOT);
    zone_work = zones.getRawPtr<double>(FID_ZW);
    zone_work_rate = zones.getRawPtr<double>(FID_ZWR);
    zone_sound_speed = zones.getRawPtr<double>(FID_ZSS);
    zone_dvel = zones.getRawPtr<double>(FID_ZDU);

    // initialize hydro vars
    for (int zch = 0; zch < numzch; ++zch) {
        int zfirst = mesh->zone_chunks_CRS[zch];
        int zlast = mesh->zone_chunks_CRS[zch+1];

        fill(&zone_rho[zfirst], &zone_rho[zlast], rho_init);
        fill(&zone_energy_density[zfirst], &zone_energy_density[zlast], energy_init);
        fill(&zone_work_rate[zfirst], &zone_work_rate[zlast], 0.);

        const double& subrgn_xmin = mesh->subregion_xmin;
        const double& subrgn_xmax = mesh->subregion_xmax;
        const double& subrgn_ymin = mesh->subregion_ymin;
        const double& subrgn_ymax = mesh->subregion_ymax;
        if (subrgn_xmin != std::numeric_limits<double>::max()) {
            const double eps = 1.e-12;
            #pragma ivdep
            for (int z = zfirst; z < zlast; ++z) {
                if (zx[z].x > (subrgn_xmin - eps) &&
                    zx[z].x < (subrgn_xmax + eps) &&
                    zx[z].y > (subrgn_ymin - eps) &&
                    zx[z].y < (subrgn_ymax + eps)) {
                    zone_rho[z]  = rho_init_sub;
                    zone_energy_density[z] = energy_init_sub;
                }
            }
        }

        #pragma ivdep
        for (int z = zfirst; z < zlast; ++z) {
        		zone_mass[z] = zone_rho[z] * zvol[z];
        		zone_energy_tot[z] = zone_energy_density[z] * zone_mass[z];
        }
    }  // for sch

    for (int pch = 0; pch < numpch; ++pch) {
        int pfirst = mesh->pt_chunks_CRS[pch];
        int plast = mesh->pt_chunks_CRS[pch+1];
        if (vel_init_radial != 0.)
            initRadialVel(vel_init_radial, pfirst, plast);
        else
            fill(&pt_vel[pfirst], &pt_vel[plast], double2(0., 0.));
    }  // for pch

}

void Hydro::allocateFields()
{
    points.addField<double2>(FID_PU);
    points.addField<double2>(FID_PU0);
    points.addField<double2>(FID_PAP);
    sides_and_corners.addField<double>(FID_CMASWT);
    sides_and_corners.addField<double2>(FID_SFP);
    sides_and_corners.addField<double2>(FID_SFQ);
    sides_and_corners.addField<double2>(FID_SFT);
    sides_and_corners.addField<double2>(FID_CFTOT);
    zones.addField<double>(FID_ZR);
    zones.addField<double>(FID_ZRP);
    zones.addField<double>(FID_ZE);
    zones.addField<double>(FID_ZP);
    zones.addField<double>(FID_ZM);
    zones.addField<double>(FID_ZETOT);
    zones.addField<double>(FID_ZW);
    zones.addField<double>(FID_ZWR);
    zones.addField<double>(FID_ZSS);
    zones.addField<double>(FID_ZDU);
}

void Hydro::initRadialVel(
        const double vel,
        const int pfirst,
        const int plast) {
    const double eps = 1.e-12;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        double pmag = length(mesh->pt_x[p]);
        if (pmag > eps)
            pt_vel[p] = vel * mesh->pt_x[p] / pmag;
        else
            pt_vel[p] = double2(0., 0.);
    }
}


TimeStep Hydro::doCycle(
            const double dt) {

    const int num_pt_chunks = mesh->num_pt_chunks();
    const int num_side_chunks = mesh->num_side_chunks();

    // Begin hydro cycle
    for (int pt_chunk = 0; pt_chunk < num_pt_chunks; ++pt_chunk) {
        int pt_first = mesh->pt_chunks_CRS[pt_chunk];
        int pt_last = mesh->pt_chunks_CRS[pt_chunk+1];

        // save off point variable values from previous cycle
        copy(&mesh->pt_x[pt_first], &mesh->pt_x[pt_last], &mesh->pt_x0[pt_first]);
        copy(&pt_vel[pt_first], &pt_vel[pt_last], &pt_vel0[pt_first]);

        // ===== Predictor step =====
        // 1. advance mesh to center of time step
        advPosHalf(dt, pt_first, pt_last);
    } // for pch

    for (int sch = 0; sch < num_side_chunks; ++sch) {
        int sfirst = mesh->side_chunks_CRS[sch];
        int slast = mesh->side_chunks_CRS[sch+1];
        int zfirst = mesh->side_zone_chunks_first(sch);
        int zlast = mesh->side_zone_chunks_last(sch);

        // save off zone variable values from previous cycle
        copy(&mesh->zone_vol[zfirst], &mesh->zone_vol[zlast], &mesh->zone_vol0[zfirst]);

        // 1a. compute new mesh geometry
        LocalMesh::calcCtrs(sfirst, slast, mesh->pt_x_pred,
                mesh->map_side2zone, mesh->num_sides, mesh->num_zones, mesh->map_side2pt1, mesh->map_side2edge, mesh->zone_pts_ptr,
                mesh->edge_x_pred, mesh->zone_x_pred);
        LocalMesh::calcVols(sfirst, slast, mesh->pt_x_pred, mesh->zone_x_pred,
                mesh->map_side2zone, mesh->num_sides, mesh->num_zones, mesh->map_side2pt1, mesh->zone_pts_ptr,
                mesh->side_area_pred, mesh->side_vol_pred, mesh->zone_area_pred, mesh->zone_vol_pred);
        mesh->calcMedianMeshSurfVecs(sch);
        mesh->calcEdgeLen(sch);
        mesh->calcCharacteristicLen(sch);

        // 2. compute point masses
        calcRho(mesh->zone_vol_pred, zone_mass, zone_rho_pred, zfirst, zlast);
        calcCrnrMass(sfirst, slast);

        // 3. compute material state (half-advanced)
        pgas->calcStateAtHalf(zone_rho, mesh->zone_vol_pred, mesh->zone_vol0, zone_energy_density, zone_work_rate, zone_mass, dt,
                zone_pressure_, zone_sound_speed, zfirst, zlast);

        // 4. compute forces
        pgas->calcForce(zone_pressure_, mesh->side_surfp, side_force_pres, sfirst, slast);
        tts->calcForce(mesh->zone_area_pred, zone_rho_pred, zone_sound_speed, mesh->side_area_pred, mesh->side_mass_frac, mesh->side_surfp, side_force_tts,
                sfirst, slast);
        qcs->calcForce(side_force_visc, sfirst, slast);
        sumCrnrForce(sfirst, slast);
    }  // for sch

    // sum corner masses, forces to points
    mesh->sumToPoints(crnr_weighted_mass, crnr_force_tot);

    CorrectorTaskArgs args;  // TODO all but dt out from loop?
    args.dt = dt;
    args.cfl = cfl;
    args.cflv = cflv;
    args.num_points = mesh->num_pts;
    args.num_sides = mesh->num_sides;
    args.num_zones = mesh->num_zones;
    args.zone_chunk_CRS = mesh->zone_chunks_CRS;
    args.side_chunk_CRS = mesh->side_chunks_CRS;
    args.point_chunk_CRS = mesh->pt_chunks_CRS;
    args.meshtype = params.meshtype;
    args.nzones_x = params.directs.nzones_x;
    args.nzones_y = params.directs.nzones_y;
    args.num_subregions = params.directs.ntasks;
    args.my_color = my_color;
    args.bcx = bcx;
    args.bcy = bcy;
    CorrectorTaskArgsSerializer serial;
    serial.archive(&args);

    CorrectorTask corrector_launcher(mesh->zones.getLRegion(),
            mesh->sides.getLRegion(),
            mesh->zone_pts.getLRegion(),
            mesh->points.getLRegion(),
            mesh->edges.getLRegion(),
            mesh->local_points_by_gid.getLRegion(),
            zones.getLRegion(),
            sides_and_corners.getLRegion(),
            points.getLRegion(),
            serial.getBitStream(), serial.getBitStreamSize());
    Future future = runtime->execute_task(ctx, corrector_launcher);
    TimeStep recommend = future.get_result<TimeStep>();
    return recommend;
}


void Hydro::advPosHalf(
        const double dt,
        const int pfirst,
        const int plast) {

    double dth = 0.5 * dt;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        mesh->pt_x_pred[p] = mesh->pt_x0[p] + pt_vel0[p] * dth;
    }
}


/*static*/
void Hydro::advPosFull(
        const double dt,
        const double2* pt_vel0,
        const double2* pt_accel,
        const double2* pt_x0,
        double2* pt_vel,
        double2* pt_x,
        const int pfirst,
        const int plast) {

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        pt_vel[p] = pt_vel0[p] + pt_accel[p] * dt;
        pt_x[p] = pt_x0[p] + 0.5 * (pt_vel[p] + pt_vel0[p]) * dt;
    }

}


void Hydro::calcCrnrMass(
        const int sfirst,
        const int slast) {
    const double* zarea = mesh->zone_area_pred;
    const double* side_mass_frac = mesh->side_mass_frac;

    #pragma ivdep
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);
        int z = mesh->map_side2zone[s];

        double m = zone_rho_pred[z] * zarea[z] * 0.5 * (side_mass_frac[s] + side_mass_frac[s3]);
        crnr_weighted_mass[s] = m;
    }
}


void Hydro::sumCrnrForce(
        const int sfirst,
        const int slast) {

    #pragma ivdep
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);

        double2 f = (side_force_pres[s] + side_force_visc[s] + side_force_tts[s]) -
                    (side_force_pres[s3] + side_force_visc[s3] + side_force_tts[s3]);
        crnr_force_tot[s] = f;
    }
}


/*static*/
void Hydro::calcAccel(
        const GenerateMesh* generate_mesh,
        const Double2Accessor pf,
        const DoubleAccessor pmass,
        double2* pt_accel,
        const int pfirst,
        const int plast) {

    const double fuzz = 1.e-99;

    #pragma ivdep
    for (int p = pfirst; p < plast; ++p) {
        ptr_t pt_ptr(generate_mesh->pointLocalToGlobalID(p));
        pt_accel[p] = pf.read(pt_ptr) / max(pmass.read(pt_ptr), fuzz);
    }

}


/*static*/
void Hydro::calcRho(
        const double* zvol,
        const double* zm,
        double* zr,
        const int zfirst,
        const int zlast)
{
    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        zr[z] = zm[z] / zvol[z];
    }
}


/*static*/
void Hydro::calcWork(
        const double dt,
        const int* map_side2pt1,
        const int* map_side2zone,
        const int* zone_pts_ptr,
        const double2* side_force_pres,
        const double2* side_force_visc,
        const double2* pt_vel,
        const double2* pt_vel0,
        const double2* pt_x_pred,
        double* zone_energy_tot,
        double* zone_work,
        const int side_first,
        const int side_last) {
    // Compute the work done by finding, for each element/node pair,
    //   dwork= force * vavg
    // where force is the force of the element on the node
    // and vavg is the average velocity of the node over the time period

    const double dth = 0.5 * dt;

    for (int side = side_first; side < side_last; ++side) {
        int p1 = map_side2pt1[side];
        int p2 = LocalMesh::mapSideToPt2(side, map_side2pt1, map_side2zone, zone_pts_ptr);
        int z = map_side2zone[side];

        double2 sftot = side_force_pres[side] + side_force_visc[side];
        double sd1 = dot( sftot, (pt_vel0[p1] + pt_vel[p1]));
        double sd2 = dot(-sftot, (pt_vel0[p2] + pt_vel[p2]));
        double dwork = -dth * (sd1 * pt_x_pred[p1].x + sd2 * pt_x_pred[p2].x);

        zone_energy_tot[z] += dwork;
        zone_work[z] += dwork;

    }

}


/*static*/
void Hydro::calcWorkRate(
        const double dt,
        const double* zone_vol,
        const double* zone_vol0,
        const double* zone_work,
        const double* zone_pressure,
        double* zone_work_rate,
        const int zfirst,
        const int zlast) {
    double dtinv = 1. / dt;
    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        double dvol = zone_vol[z] - zone_vol0[z];
        zone_work_rate[z] = (zone_work[z] + zone_pressure[z] * dvol) * dtinv;
    }

}


/*static*/
void Hydro::calcEnergy(
        const double* zone_energy_tot,
        const double* zone_mass,
        double* zone_energy_density,
        const int zfirst,
        const int zlast)
{
    const double fuzz = 1.e-99;
    #pragma ivdep
    for (int z = zfirst; z < zlast; ++z) {
        zone_energy_density[z] = zone_energy_tot[z] / (zone_mass[z] + fuzz);
    }
}


void Hydro::sumEnergy(
        const double* zetot,
        const double* zarea,
        const double* zvol,
        const double* zm,
        const double* side_mass_frac,
        const double2* px,
        const double2* pu,
        double& ei,
        double& ek,
        const int zfirst,
        const int zlast,
        const int sfirst,
        const int slast) {

    // compute internal energy
    double sumi = 0.; 
    for (int z = zfirst; z < zlast; ++z) {
        sumi += zetot[z];
    }
    // multiply by 2\pi for cylindrical geometry
    ei += sumi * 2 * M_PI;

    // compute kinetic energy
    // in each individual zone:
    // zone ke = zone mass * (volume-weighted average of .5 * u ^ 2)
    //         = zm sum(c in z) [cvol / zvol * .5 * u ^ 2]
    //         = sum(c in z) [zm * cvol / zvol * .5 * u ^ 2]
    double sumk = 0.; 
    for (int s = sfirst; s < slast; ++s) {
        int s3 = mesh->mapSideToSidePrev(s);
        int p1 = mesh->map_side2pt1[s];
        int z = mesh->map_side2zone[s];

        double cvol = zarea[z] * px[p1].x * 0.5 * (side_mass_frac[s] + side_mass_frac[s3]);
        double cke = zm[z] * cvol / zvol[z] * 0.5 * length2(pu[p1]);
        sumk += cke;
    }
    // multiply by 2\pi for cylindrical geometry
    ek += sumk * 2 * M_PI;

}


/*static*/
void Hydro::calcDtCourant(
        double& dtrec,
        char* msgdtrec,
        const int zfirst,
        const int zlast,
        const double* zdl,
        const double* zone_dvel,
        const double* zone_sound_speed,
        const double cfl)
{
    const double fuzz = 1.e-99;
    double dtnew = 1.e99;
    int zmin = -1;
    for (int z = zfirst; z < zlast; ++z) {
        double cdu = std::max(zone_dvel[z], std::max(zone_sound_speed[z], fuzz));
        double zdthyd = zdl[z] * cfl / cdu;
        zmin = (zdthyd < dtnew ? z : zmin);
        dtnew = (zdthyd < dtnew ? zdthyd : dtnew);
    }

    if (dtnew < dtrec) {
        dtrec = dtnew;
        snprintf(msgdtrec, 80, "Hydro Courant limit for z = %d", zmin);
    }

}


/*static*/
void Hydro::calcDtVolume(
        const double dtlast,
        double& dtrec,
        char* msgdtrec,
        const int zfirst,
        const int zlast,
        const double* zvol,
        const double* zvol0,
        const double cflv)
{
    double dvovmax = 1.e-99;
    int zmax = -1;
    for (int z = zfirst; z < zlast; ++z) {
        double zdvov = std::abs((zvol[z] - zvol0[z]) / zvol0[z]);
        zmax = (zdvov > dvovmax ? z : zmax);
        dvovmax = (zdvov > dvovmax ? zdvov : dvovmax);
    }
    double dtnew = dtlast * cflv / dvovmax;
    if (dtnew < dtrec) {
        dtrec = dtnew;
        snprintf(msgdtrec, 80, "Hydro dV/V limit for z = %d", zmax);
    }

}


/*static*/
void Hydro::calcDtHydro(
        const double dtlast,
        const int zfirst,
        const int zlast,
        const double* zone_dl,
        const double* zone_dvel,
        const double* zone_sound_speed,
        const double cfl,
        const double* zone_vol,
        const double* zone_vol0,
        const double cflv,
        TimeStep& recommend)
{
    double dtchunk = 1.e99;
    char msgdtchunk[80];

    calcDtCourant(dtchunk, msgdtchunk, zfirst, zlast, zone_dl,
            zone_dvel, zone_sound_speed, cfl);
    calcDtVolume(dtlast, dtchunk, msgdtchunk, zfirst, zlast,
            zone_vol, zone_vol0, cflv);
    if (dtchunk < recommend.dt) {
        {
            // redundant test needed to avoid race condition
            if (dtchunk < recommend.dt) {
                recommend.dt = dtchunk;
                strncpy(recommend.message, msgdtchunk, 80);
            }
        }
    }

}


void Hydro::writeEnergyCheck() {

    double ei = 0.;
    double ek = 0.;
    for (int sch = 0; sch < mesh->num_side_chunks(); ++sch) {
        int sfirst = mesh->side_chunks_CRS[sch];
        int slast = mesh->side_chunks_CRS[sch+1];
        int zfirst = mesh->side_zone_chunks_first(sch);
        int zlast = mesh->side_zone_chunks_last(sch);

        double eichunk = 0.;
        double ekchunk = 0.;
        sumEnergy(zone_energy_tot, mesh->zone_area, mesh->zone_vol, zone_mass, mesh->side_mass_frac,
                mesh->pt_x, pt_vel, eichunk, ekchunk,
                zfirst, zlast, sfirst, slast);
        {
            ei += eichunk;
            ek += ekchunk;
        }
    }


    Future future_sum = Parallel::globalSum(ei, add_reduction, runtime, ctx);
    ei = future_sum.get_result<double>();

    future_sum = Parallel::globalSum(ek, add_reduction, runtime, ctx);
    ek = future_sum.get_result<double>();

    if (my_color == 0) {
        cout << scientific << setprecision(6);
        cout << "Energy check:  "
             << "total energy  = " << setw(14) << ei + ek << endl;
        cout << "(internal = " << setw(14) << ei
             << ", kinetic = " << setw(14) << ek << ")" << endl;
    }

}

void Hydro::copyZonesToLegion(
        DoubleAccessor* rho_acc,
        DoubleAccessor*  energy_density_acc,
        DoubleAccessor*  pressure_acc,
        IndexSpace ispace_zones)
{
    IndexIterator zone_itr(runtime,ctx, ispace_zones);  // TODO continue to investigate why passing LogicalUnstructured in failed
    int z = 0;
    while (zone_itr.has_next()) {
        ptr_t zone_ptr = zone_itr.next();
        rho_acc->write(zone_ptr, zone_rho[z]);
        energy_density_acc->write(zone_ptr, zone_energy_density[z]);
        pressure_acc->write(zone_ptr, zone_pressure_[z]);
        z++;
    }
    assert(z == mesh->num_zones);
}
