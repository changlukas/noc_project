"""Calibrated, whole-transfer scheduling model for fixed-XY DMA traffic.

This is a transport extension, not the paper's original CE cycle equation.
Payload service reserves endpoint directions and traversed directed links;
response/command overhead remains in the calibrated completion tail. Blocking,
credit latency, packet boundaries and out-of-order effects are residuals.
"""
import itertools
import math


def axis(a, b):
    return "east" if b-a==1 else "west" if b-a==-1 else "north" if b-a==4 else "south"


def solve(matrix, vector):
    a=[list(row)+[v] for row,v in zip(matrix,vector)]
    for col in range(len(a)):
        pivot=max(range(col,len(a)), key=lambda r:abs(a[r][col]))
        a[col],a[pivot]=a[pivot],a[col]
        if abs(a[col][col])<1e-12:
            raise ValueError("calibration features are not identifiable")
        scale=a[col][col]
        a[col]=[x/scale for x in a[col]]
        for row in range(len(a)):
            if row!=col:
                scale=a[row][col]
                a[row]=[x-scale*y for x,y in zip(a[row],a[col])]
    return [row[-1] for row in a]


def fit(samples):
    """Nonnegative least squares for alpha + tau*hops + rho*payload bytes."""
    scale=max(q for q,h,t in samples)
    rows=[[1,h,q/scale] for q,h,t in samples]
    target=[t for q,h,t in samples]
    best=None
    for size in (1,2,3):
        for ids in itertools.combinations(range(3),size):
            matrix=[[sum(r[i]*r[j] for r in rows) for j in ids] for i in ids]
            vector=[sum(r[i]*y for r,y in zip(rows,target)) for i in ids]
            try:
                values=solve(matrix,vector)
            except ValueError:
                continue
            if any(v<0 for v in values):
                continue
            coeff=[0.0]*3
            for i,v in zip(ids,values):
                coeff[i]=v
            error=sum((sum(x*c for x,c in zip(r,coeff))-y)**2 for r,y in zip(rows,target))
            if best is None or error<best[0]:
                best=error,coeff
    if best is None or best[1][2]<=0:
        raise ValueError("invalid calibrated service capacity")
    _,(alpha,tau,rho)=best
    return dict(completion_overhead_cycles=alpha,hop_cycles=tau,
                service_bytes_per_cycle=scale/rho,fit_squared_error=best[0])


def estimate(contract, parameters, endpoint_mode="independent_directions"):
    jobs=contract["costs"]["transfers"]
    if not jobs:
        raise ValueError("empty transfer contract")
    pending=list(jobs)
    ends,starts,resources={}, {}, {}
    while pending:
        candidates=[]
        for index,job in enumerate(pending):
            key=(job["issuer"],job["job"])
            dependencies=[tuple(k) for k in job["prerequisites"]]
            previous=(key[0],key[1]-1)
            if any(k not in ends for k in dependencies) or (key[1]>1 and previous not in starts):
                continue
            path=job["path"]
            edges=list(zip(path,path[1:]))
            profiles=[parameters["directions"][axis(a,b)] for a,b in edges]
            alpha=sum(x["completion_overhead_cycles"] for x in profiles)/len(profiles)
            hops=sum(x["hop_cycles"] for x in profiles)
            service=job.get("service_bytes",job["bytes"])/min(x["service_bytes_per_cycle"] for x in profiles)
            if endpoint_mode=="shared_send_receive":
                ports=[("endpoint",path[0]),("endpoint",path[-1])]
            else:
                ports=[("send",path[0]),("receive",path[-1])]
            used=ports+[("link",a,b) for a,b in edges]
            start=max([0.0]+[ends[k] for k in dependencies]+[resources.get(k,0.0) for k in used]
                      +([starts[previous]+parameters["driver_issue_cycles"]] if key[1]>1 else []))
            candidates.append((start,key,index,service,alpha+hops,used))
        if not candidates:
            raise ValueError("cyclic or incomplete model dependencies")
        start,key,index,service,tail,used=min(candidates,key=lambda c:(c[0],c[1]))
        starts[key]=start
        ends[key]=start+service+tail
        for resource in used:
            resources[resource]=start+service
        pending.pop(index)
    result=max(ends.values())
    if not math.isfinite(result) or result<=0:
        raise ValueError("invalid estimated completion time")
    return result


def calibrate(records):
    groups={direction:[] for direction in ("east","west","north","south")}
    fit_records,duplex,heldout=[],[],[]
    for record in records:
        contract=record.get("contract",{})
        kind=contract.get("variant","")
        if not kind.startswith(("xy_cal_","xy_duplex_")):
            continue
        jobs=contract["costs"]["transfers"]
        if kind.startswith("xy_duplex_"):
            duplex.append(record)
            continue
        job=jobs[0]
        path=job["path"]
        if len(path)>3:
            heldout.append(record)
            continue
        directions={axis(a,b) for a,b in zip(path,path[1:])}
        if len(directions)!=1:
            raise ValueError("unexpected calibration path")
        groups[directions.pop()].append((job["bytes"],len(path)-1,record["duration_cycles"]))
        fit_records.append(record)
    if any(len(samples)!=5 for samples in groups.values()) or len(duplex)!=6 or len(heldout)!=2:
        raise ValueError("incomplete independent calibration matrix")
    model=dict(schema=1,model_id="xy_dma_resource_service_v1",
               directions={key:fit(samples) for key,samples in groups.items()},
               driver_issue_cycles=2,
               driver_issue_source="idma_job_driver.sv VALID pulse and next job negedge",
               assumptions=["whole-transfer response dependencies", "AXI-beat-rounded resource serialization",
                            "calibrated command/response completion tail", "fixed XY",
                            "no injected random stalls", "traffic only; no CE compute"],
               limitations=["packet/header and credit costs are absorbed only through isolated calibration",
                            "heterogeneous endpoint effects and queue arbitration are not cycle-exact",
                            "directional service parameters describe aggregate transport, not individual units"])
    errors={}
    for mode in ("independent_directions","shared_send_receive"):
        errors[mode]=sum((estimate(r["contract"],model,mode)-r["duration_cycles"])**2 for r in duplex)
    model["endpoint_mode"]=min(errors,key=errors.get)
    model["duplex_model_squared_errors"]=errors
    def evaluation(rows):
        return [dict(directory=str(r["directory"]),execution_time_cycles=r["duration_cycles"],
                     estimated_time_cycles=estimate(r["contract"],model,model["endpoint_mode"])) for r in rows]
    model["calibration"]=evaluation(fit_records+duplex)
    model["heldout"]=evaluation(heldout)
    return model
