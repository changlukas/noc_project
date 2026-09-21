"""Evidence-boundary and failure tests for the original SMC mapping."""
from copy import deepcopy
import pytest
from xy_smc_analysis import analyze


def evidence():
    contract={"variant":"direct_xy","routing":"XY","costs":{"transfers":[
        {"path":[0,1],"service_bytes":64,"prerequisites":[]}]}}
    txn={"node":0,"direction":"AW","status":"complete","in_window":True,
         "request_plane":"DAT","accepted_cycle":10,"header_injection_cycle":12,
         "completion_cycle":100,"beats":1,"size":6,
         "sam_destination":{"x":1,"y":0,"port":0}}
    packet={"source":0,"destination":1,"plane":"DAT","in_window":True,
            "injection_head":12,"ejection_tail":20,"flit_count":2}
    packets={"deliveries":[packet],"incomplete_packets":[],"missing_deliveries":[]}
    return contract,[txn],packets,[{"name":"dat_0to1","flit_count":2}]


def test_boundary_excludes_response_and_includes_ni_admission():
    args=evidence();r=analyze(*args)
    assert r["execution_cycles"]==11
    assert r["estimated_cycles"]==8  # max(2, 2/1+1)+(2*2+1)*1
    args[1][0]["completion_cycle"]=1000
    assert analyze(*args)==r


def test_response_packets_do_not_contribute_to_smc():
    args=evidence();expected=analyze(*args)
    args[2]["deliveries"].append({"plane":"RSP","ejection_tail":500})
    assert analyze(*args)==expected


@pytest.mark.parametrize("fault",["missing","duplicate","owner","count","link","coverage","dependency","early","unaccepted"])
def test_invalid_evidence_rejected(fault):
    args=evidence();c,tx,pk,links=args
    if fault=="missing": pk["deliveries"].clear()
    if fault=="duplicate":pk["deliveries"].append(deepcopy(pk["deliveries"][0]))
    if fault=="owner":pk["deliveries"][0]["destination"]=2
    if fault=="count":pk["deliveries"][0]["flit_count"]=1
    if fault=="link":links[0]["flit_count"]=1
    if fault=="coverage":c["costs"]["transfers"][0]["service_bytes"]=128
    if fault=="dependency":c["costs"]["transfers"][0]["prerequisites"]=[[0,1]]
    if fault=="early":tx[0]["accepted_cycle"]=13
    if fault=="unaccepted":tx[0]["status"]="incomplete"
    with pytest.raises(ValueError):analyze(*args)


def test_uneven_link_loading_preserves_original_average_term():
    c,tx,pk,links=evidence()
    c["costs"]["transfers"].append({"path":[2,1],"service_bytes":64,"prerequisites":[]})
    other=deepcopy(tx[0]);other.update(node=2,accepted_cycle=15,header_injection_cycle=17)
    tx.append(other)
    other=deepcopy(pk["deliveries"][0]);other.update(source=2,injection_head=17,ejection_tail=30)
    pk["deliveries"].append(other)
    links.append({"name":"dat_2to1","flit_count":2})
    result=analyze(c,tx,pk,links)
    assert (result["C_flits"],result["E_flit_hops"],result["N_directed_links"])==(4,4,2)
    assert result["estimated_cycles"]==9
    assert result["execution_cycles"]==21
